// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "python_object_binder.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "native/image_annotations_ops.h"
#include "native/image_ops.h"
#include "native/scene_entities_ops.h"
#include "pj_base/builtin/field_table_registry.hpp"
#include "pj_scripting/memory_budget.h"
#include "pj_scripting/object_script.h"
#include "script_contract.h"
#include "script_media.h"

namespace PJ::scripting::detail {
namespace {

thread_local PythonRun* t_run = nullptr;

PythonRun& currentRun() {
  if (t_run == nullptr) {
    throw std::runtime_error("pj objects work only inside a run");
  }
  return *t_run;
}

// ---- boxes --------------------------------------------------------------------

/// What keeps an object alive for the script: the value, what the builder paid for a builder's output, and for an
/// input what keeps the code that made it loaded (`NamedValue::keepalive`), released after the value. Every view read
/// from the object shares it, so none lapses while a view, or a script that kept one past its run, can still read it.
struct Anchor {
  std::shared_ptr<const void> keepalive;
  sdk::BuiltinObject object;
  std::shared_ptr<const native::MemoryReservation> charge;
};
using Keep = std::shared_ptr<const Anchor>;

/// A read-only view of the struct at `owner`, described by `table`: a field or list element of an object, or the object
/// itself, whose `owner` is its whole value.
struct StructView {
  Keep keep;
  const sdk::FieldTableView* table = nullptr;
  const void* owner = nullptr;
};

/// A list field, read element by element through `list_at` (never materialized).
struct ListView {
  Keep keep;
  const sdk::FieldDescriptor* list = nullptr;
  const void* owner = nullptr;
};

/// `busy` while a scan runs without the GIL: another thread may not advance the iterator meanwhile.
struct PointIterator {
  Keep cloud;
  native::XyzAccess access;
  std::size_t next = 0;
  bool busy = false;
};

/// A builder, empty once `finish()` spent it. It spends the budget of the run that made it (`run`, its generation), so
/// no later run may use it.
template <class Builder>
struct BuilderSlot {
  std::uint64_t run = 0;
  std::optional<Builder> builder;
};
using Scene = native::SceneEntityBuilder;
using Annotations = native::ImageAnnotationsBuilder;

/// The value of `pj.scene`, `pj.annotations` and `pj.tf`, which hold nothing.
struct Unit {};

// Every type is a plain CPython type: CPython's own argument parsing and allocation fail cleanly where pybind11's
// dispatcher and instance creation dereference a refused allocation, and a list element costs no type lookup.
PyTypeObject* g_object_type = nullptr;
PyTypeObject* g_struct_type = nullptr;
PyTypeObject* g_list_type = nullptr;
PyTypeObject* g_points_type = nullptr;
PyTypeObject* g_scene_type = nullptr;
PyTypeObject* g_annotations_type = nullptr;

/// A C++ value inside a Python object of a plain type.
template <class Value>
struct Boxed {
  PyObject_HEAD Value value;
};

template <class Value>
py::object newBoxed(PyTypeObject* type, Value value) {
  auto* self = PyObject_New(Boxed<Value>, type);
  if (self == nullptr) {
    throw py::error_already_set();
  }
  new (&self->value) Value(std::move(value));
  return py::reinterpret_steal<py::object>(reinterpret_cast<PyObject*>(self));
}

template <class Value>
Value& unboxed(PyObject* self) {
  return reinterpret_cast<Boxed<Value>*>(self)->value;
}

template <class Value>
void destroyBoxed(PyObject* self) {
  PyTypeObject* type = Py_TYPE(self);
  unboxed<Value>(self).~Value();
  PyObject_Free(self);
  Py_DECREF(type);
}

/// A C slot's body: an exception becomes the pending Python error and the slot's null.
template <class Body>
auto slotCall(Body&& body) -> decltype(body()) {
  try {
    return body();
  } catch (py::error_already_set& error) {
    error.restore();
  } catch (const py::builtin_exception& error) {
    error.set_error();
  } catch (const std::exception& error) {
    PyErr_SetString(PyExc_RuntimeError, error.what());
  }
  return {};
}

/// A method `Body(self, args, kwargs)` as a `METH_VARARGS | METH_KEYWORDS` C function.
template <py::object (*Body)(PyObject*, PyObject*, PyObject*)>
PyObject* withArguments(PyObject* self, PyObject* args, PyObject* kwargs) {
  return slotCall([&]() -> PyObject* { return Body(self, args, kwargs).release().ptr(); });
}

/// A method `Body(self)` as a `METH_NOARGS` C function.
template <py::object (*Body)(PyObject*)>
PyObject* withoutArguments(PyObject* self, PyObject* /*unused*/) {
  return slotCall([&]() -> PyObject* { return Body(self).release().ptr(); });
}

template <class Function>
PyCFunction cfunction(Function* function) {
  return reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)()>(function));
}

/// Parses a method's arguments with CPython's parser by `format` and the null-terminated `names`; a mismatch raises.
template <class... Outputs>
void parse(PyObject* args, PyObject* kwargs, const char* format, const char* const* names, Outputs*... outputs) {
  if (PyArg_ParseTupleAndKeywords(args, kwargs, format, const_cast<char**>(names), outputs...) == 0) {
    throw py::error_already_set();
  }
}

py::object newFloat(double value) {
  return checked(PyFloat_FromDouble(value));
}

/// A tuple of `items`, built with checked CPython calls; each item moves into its slot.
template <class... Items>
py::object newTuple(Items... items) {
  py::object tuple = checked(PyTuple_New(static_cast<Py_ssize_t>(sizeof...(items))));
  Py_ssize_t index = 0;
  (PyTuple_SET_ITEM(tuple.ptr(), index++, items.release().ptr()), ...);
  return tuple;
}

/// A dict of `items`, built with checked CPython calls.
py::object newDict(std::initializer_list<std::pair<const char*, py::object>> items) {
  py::object dict = checked(PyDict_New());
  for (const auto& [key, value] : items) {
    if (PyDict_SetItemString(dict.ptr(), key, value.ptr()) != 0) {
      throw py::error_already_set();
    }
  }
  return dict;
}

py::object wrapObject(
    sdk::BuiltinObject object, std::shared_ptr<const native::MemoryReservation> charge,
    std::shared_ptr<const void> keepalive = {}) {
  const sdk::FieldTableView* table = sdk::describe(object.type());
  const void* data = structPointer(object);
  return newBoxed(
      g_object_type,
      StructView{
          std::make_shared<const Anchor>(Anchor{std::move(keepalive), std::move(object), std::move(charge)}), table,
          data});
}

template <class Value>
py::object newCharged(native::Charged<Value> done) {
  return wrapObject(
      sdk::BuiltinObject(std::move(done.value)),
      std::make_shared<const native::MemoryReservation>(std::move(done.charge)));
}

// ---- native calls -------------------------------------------------------------

/// Runs a native operation without the GIL: it reads only C++ state its caller's arguments keep alive.
template <class Operation>
auto withoutGil(Operation&& operation) {
  py::gil_scoped_release no_gil;
  return operation();
}

/// `withoutGil` for an allocating operation: one that fails on the native budget runs a full collection, which frees
/// the outputs dead in reference cycles, and is retried once.
template <class Operation>
auto released(Operation&& operation) {
  auto result = withoutGil(operation);
  if (!result && result.error().starts_with(native::kNativeLimitError)) {
    PyGC_Collect();
    return withoutGil(operation);
  }
  return result;
}

/// The value of `result`, else its error raised as the script's `ValueError`.
template <class T>
T valueOf(Expected<T> result) {
  if (!result) {
    throw py::value_error(result.error());
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

const native::NativeLimits& limits() {
  return currentRun().media.limits();
}

// ---- reading arguments --------------------------------------------------------------
//
// An argument converts before the method touches its receiver: script code a conversion runs (a sequence's
// `__len__`, a number's `__float__`) cannot spend a builder the call then uses.

/// A tuple snapshot of the sequence `value`, so the conversions that follow, which may run script code, read storage
/// nothing else can change. `subject` names the argument in the error, already quoted (`'center'`, `'points' entry`);
/// the text is built only on failure, since this runs once per point.
py::object sequenceOf(py::handle value, const char* subject) {
  py::object items = py::reinterpret_steal<py::object>(PySequence_Tuple(value.ptr()));
  if (!items) {
    PyErr_Clear();
    throw py::value_error(std::string(subject) + " must be a sequence");
  }
  return items;
}

template <std::size_t N>
std::array<double, N> readNumbers(py::handle value, const char* subject) {
  const auto notNumbers = [&] {
    return py::value_error(std::string(subject) + " must be a sequence of " + std::to_string(N) + " numbers");
  };
  const py::object items = sequenceOf(value, subject);
  if (PyTuple_GET_SIZE(items.ptr()) != static_cast<Py_ssize_t>(N)) {
    throw notNumbers();
  }
  std::array<double, N> numbers{};
  for (std::size_t index = 0; index < N; ++index) {
    numbers[index] = PyFloat_AsDouble(PyTuple_GET_ITEM(items.ptr(), static_cast<Py_ssize_t>(index)));
    if (numbers[index] == -1.0 && PyErr_Occurred() != nullptr) {
      PyErr_Clear();
      throw notNumbers();
    }
  }
  return numbers;
}

template <class Point, std::size_t N>
Point readPoint(py::handle value, const char* subject) {
  return std::apply([](auto... coordinates) { return Point{coordinates...}; }, readNumbers<N>(value, subject));
}

/// The points, charged to the run's native budget before they are allocated; the caller hands the charge over to the
/// builder, which charges what it keeps.
template <class Point, std::size_t N>
native::Charged<std::vector<Point>> readPoints(py::handle value, const char* name) {
  const std::string subject = std::string("'") + name + "'";
  const py::object items = sequenceOf(value, subject.c_str());
  const std::string entry = subject + " entry";
  const auto count = static_cast<std::size_t>(PyTuple_GET_SIZE(items.ptr()));
  native::MemoryReservation charge(limits().budget);
  valueOf(charge.grow(count * sizeof(Point)));
  std::vector<Point> points;
  points.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    points.push_back(readPoint<Point, N>(PyTuple_GET_ITEM(items.ptr(), static_cast<Py_ssize_t>(index)), entry.c_str()));
  }
  return {std::move(points), std::move(charge)};
}

/// `(r, g, b)` or `(r, g, b, a)`, components 0-255 (alpha 255 when absent); `fallback` for None or absent.
sdk::ColorRGBA readColor(PyObject* value, const char* name, sdk::ColorRGBA fallback) {
  if (value == nullptr || value == Py_None) {
    return fallback;
  }
  const std::string subject = std::string("'") + name + "'";
  const py::object items = sequenceOf(value, subject.c_str());
  const Py_ssize_t count = PyTuple_GET_SIZE(items.ptr());
  if (count != 3 && count != 4) {
    throw py::value_error(std::string("'") + name + "' must be (r, g, b) or (r, g, b, a)");
  }
  std::array<uint8_t, 4> channels{0, 0, 0, 255};
  for (Py_ssize_t index = 0; index < count; ++index) {
    const double channel = PyFloat_AsDouble(PyTuple_GET_ITEM(items.ptr(), index));
    PyErr_Clear();
    if (!(channel >= 0.0 && channel <= 255.0)) {
      throw py::value_error(std::string("'") + name + "' components must be numbers from 0 to 255");
    }
    channels[static_cast<std::size_t>(index)] = static_cast<uint8_t>(channel);
  }
  return {channels[0], channels[1], channels[2], channels[3]};
}

/// A number argument, `fallback` for None or absent.
double numberOr(PyObject* value, double fallback) {
  if (value == nullptr || value == Py_None) {
    return fallback;
  }
  const double number = PyFloat_AsDouble(value);
  if (number == -1.0 && PyErr_Occurred() != nullptr) {
    throw py::error_already_set();
  }
  return number;
}

native::CompareOp comparison(const char* op, const char* what) {
  const std::optional<native::CompareOp> parsed = parseComparison(op, "!=");
  if (!parsed) {
    throw py::value_error(std::string(what) + ": unknown comparison '" + op + "' (expected <, <=, >, >=, ==, !=)");
  }
  return *parsed;
}

// ---- fields -------------------------------------------------------------------

py::object scalar(sdk::FieldKind kind, const sdk::FieldDescriptor& field, const void* pointer) {
  switch (kind) {
    case sdk::FieldKind::kBool:
      return py::bool_(field.get_number(pointer) != 0.0);
    case sdk::FieldKind::kInt64:
      return checked(PyLong_FromLongLong(field.get_int64(pointer)));
    case sdk::FieldKind::kNumber:
    case sdk::FieldKind::kEnum:
      return newFloat(field.get_number(pointer));
    case sdk::FieldKind::kString:
      return newString(field.get_string(pointer));
    default:
      return py::none();
  }
}

py::object fieldValue(const sdk::FieldDescriptor& field, const void* owner, const Keep& keep) {
  switch (field.kind) {
    case sdk::FieldKind::kStruct:
      return newBoxed(g_struct_type, StructView{keep, field.nested, field.struct_ptr(owner)});
    case sdk::FieldKind::kList:
      return newBoxed(g_list_type, ListView{keep, &field, owner});
    case sdk::FieldKind::kBuffer:
      throw py::attribute_error("field '" + std::string(field.name) + "' is a raw buffer; use the object's methods");
    case sdk::FieldKind::kOptionalNumber:
      return field.has_value(owner) ? newFloat(field.get_number(owner)) : py::none();
    default:
      return scalar(field.kind, field, owner);
  }
}

/// The field `name` names, or null when the table has none.
py::object namedField(const sdk::FieldTableView& table, const void* owner, const Keep& keep, PyObject* name) {
  Py_ssize_t length = 0;
  const char* text = PyUnicode_Check(name) != 0 ? PyUnicode_AsUTF8AndSize(name, &length) : nullptr;
  if (text == nullptr) {
    throw py::error_already_set();
  }
  const std::string_view key(text, static_cast<std::size_t>(length));
  for (const sdk::FieldDescriptor& field : table.fields) {
    if (field.name == key) {
      return fieldValue(field, owner, keep);
    }
  }
  return {};
}

/// `tp_getattro` of a view: its fields; an object then falls back to its type's methods (`kMethods`), any other name of
/// a field's view is an error.
template <bool kMethods>
PyObject* attribute(PyObject* self, PyObject* name) {
  return slotCall([&]() -> PyObject* {
    const StructView& view = unboxed<StructView>(self);
    py::object field = namedField(*view.table, view.owner, view.keep, name);
    if (field) {
      return field.release().ptr();
    }
    if constexpr (kMethods) {
      return PyObject_GenericGetAttr(self, name);
    } else {
      PyErr_Format(PyExc_AttributeError, "no field '%U'", name);
      return nullptr;
    }
  });
}

Py_ssize_t listLength(PyObject* self) {
  const ListView& view = unboxed<ListView>(self);
  return static_cast<Py_ssize_t>(view.list->list_size(view.owner));
}

/// `sq_item` of a list view: CPython has already added the length to a negative index.
PyObject* listItem(PyObject* self, Py_ssize_t index) {
  const ListView& view = unboxed<ListView>(self);
  if (index < 0 || index >= listLength(self)) {
    PyErr_SetString(PyExc_IndexError, "list index out of range");
    return nullptr;
  }
  return slotCall([&]() -> PyObject* {
    const void* element = view.list->list_at(view.owner, static_cast<std::size_t>(index));
    return (view.list->nested != nullptr ? newBoxed(g_struct_type, StructView{view.keep, view.list->nested, element})
                                         : scalar(view.list->element_kind, *view.list, element))
        .release()
        .ptr();
  });
}

// ---- object methods ---------------------------------------------------------------

/// The object `self` (an object proxy) holds.
const sdk::BuiltinObject& heldObject(PyObject* self) {
  return unboxed<StructView>(self).keep->object;
}

const sdk::PointCloud& cloudOf(PyObject* self) {
  const auto* cloud = heldObject(self).get<sdk::PointCloud>();
  if (cloud == nullptr) {
    throw py::type_error("point cloud expected");
  }
  return *cloud;
}

py::object newCloud(Expected<sdk::PointCloud> cloud) {
  return wrapObject(sdk::BuiltinObject(valueOf(std::move(cloud))), nullptr);
}

py::object vector(double x, double y, double z) {
  return newTuple(newFloat(x), newFloat(y), newFloat(z));
}

/// The points of a cloud, or of every points annotation.
py::object count(PyObject* self) {
  if (const auto* annotations = heldObject(self).get<sdk::ImageAnnotations>()) {
    return checked(PyLong_FromSize_t(native::countAnnotationPoints(*annotations)));
  }
  return checked(PyLong_FromUnsignedLongLong(native::countPoints(cloudOf(self))));
}

py::object bounds(PyObject* self) {
  const sdk::PointCloud& cloud = cloudOf(self);
  const native::Bounds found = valueOf(released([&] { return native::bounds(cloud); }));
  if (found.count == 0) {
    return py::none();
  }
  return newDict(
      {{"min", vector(found.min.x, found.min.y, found.min.z)},
       {"max", vector(found.max.x, found.max.y, found.max.z)},
       {"count", checked(PyLong_FromSize_t(found.count))}});
}

py::object extremePoint(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"channel", "which", nullptr};
  const char* channel = nullptr;
  const char* which = nullptr;
  parse(args, kwargs, "ss:extreme_point", kNames, &channel, &which);
  const sdk::PointCloud& cloud = cloudOf(self);
  const std::optional<native::Extreme> extreme = parseExtreme(which);
  if (!extreme) {
    throw py::value_error("extreme_point: the second argument must be 'max' or 'min'");
  }
  const auto best = valueOf(released([&] { return native::extremePoint(cloud, channel, *extreme); }));
  if (!best) {
    return py::none();
  }
  return newDict(
      {{"index", checked(PyLong_FromSize_t(best->point.index))},
       {"x", newFloat(best->point.position.x)},
       {"y", newFloat(best->point.position.y)},
       {"z", newFloat(best->point.position.z)},
       {"value", newFloat(best->value)}});
}

py::object cropBox(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"min", "max", nullptr};
  PyObject* min_value = nullptr;
  PyObject* max_value = nullptr;
  parse(args, kwargs, "OO:crop_box", kNames, &min_value, &max_value);
  const auto min = readPoint<sdk::Vector3, 3>(min_value, "'min'");
  const auto max = readPoint<sdk::Vector3, 3>(max_value, "'max'");
  const sdk::PointCloud& cloud = cloudOf(self);
  return newCloud(released([&] { return native::cropBox(cloud, min, max, limits()); }));
}

py::object filter(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"channel", "op", "value", nullptr};
  const char* channel = nullptr;
  const char* op = nullptr;
  double threshold = 0.0;
  parse(args, kwargs, "ssd:filter", kNames, &channel, &op, &threshold);
  const native::CompareOp compare = comparison(op, "filter");
  const sdk::PointCloud& cloud = cloudOf(self);
  return newCloud(released([&] { return native::filterByField(cloud, channel, compare, threshold, limits()); }));
}

/// `tf` as `pj.tf.lookup` returns it: the result is in `tf["target_frame"]` when it is a string.
py::object transform(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"tf", nullptr};
  PyObject* tf = nullptr;
  parse(args, kwargs, "O!:transform", kNames, &PyDict_Type, &tf);
  // Strong references, all taken before a conversion runs script code that may edit the dict.
  const auto value = [&](const char* key) { return py::reinterpret_borrow<py::object>(PyDict_GetItemString(tf, key)); };
  const py::object translation = value("translation");
  const py::object rotation = value("rotation");
  const py::object target = value("target_frame");
  if (!translation || !rotation) {
    throw py::value_error("transform: expected 'translation' and 'rotation', as pj.tf.lookup returns them");
  }
  sdk::FrameTransform motion;
  motion.translation = readPoint<sdk::Vector3, 3>(translation, "'translation'");
  motion.rotation = readPoint<sdk::Quaternion, 4>(rotation, "'rotation'");
  if (target && PyUnicode_Check(target.ptr()) != 0) {
    Py_ssize_t length = 0;
    const char* frame = PyUnicode_AsUTF8AndSize(target.ptr(), &length);
    if (frame == nullptr) {
      throw py::error_already_set();
    }
    motion.parent_frame_id.assign(frame, static_cast<std::size_t>(length));  // charged with the output
  }
  const sdk::PointCloud& cloud = cloudOf(self);
  return newCloud(released([&] { return native::transformXyz(cloud, motion, limits()); }));
}

py::object xyzPoints(PyObject* self) {
  const native::XyzAccess access = valueOf(native::resolveXyz(cloudOf(self)));
  return newBoxed(g_points_type, PointIterator{unboxed<StructView>(self).keep, access});
}

/// `tp_iternext` of `xyz_points()`: `(index, x, y, z)`; null without an error once the points run out. A scan past
/// `kScanWithGil` non-finite records runs without the GIL: such a run may be the whole cloud.
PyObject* nextPoint(PyObject* self) {
  constexpr std::size_t kScanWithGil = 256;
  PointIterator& iterator = unboxed<PointIterator>(self);
  if (iterator.busy) {
    PyErr_SetString(PyExc_RuntimeError, "xyz_points: the iterator is already advancing");
    return nullptr;
  }
  const sdk::PointCloud& cloud = *iterator.cloud->object.get<sdk::PointCloud>();
  auto point = native::nextFinitePoint(cloud, iterator.access, iterator.next, kScanWithGil);
  if (!point && iterator.next < iterator.access.records) {
    iterator.busy = true;
    point = withoutGil([&] { return native::nextFinitePoint(cloud, iterator.access, iterator.next); });
    iterator.busy = false;
  }
  if (!point) {
    return nullptr;
  }
  return slotCall([&]() -> PyObject* {
    return newTuple(
               checked(PyLong_FromSize_t(point->index)), newFloat(point->position.x), newFloat(point->position.y),
               newFloat(point->position.z))
        .release()
        .ptr();
  });
}

// ---- builders -------------------------------------------------------------------

constexpr sdk::ColorRGBA kWhite{255, 255, 255, 255};
constexpr sdk::ColorRGBA kTransparent{0, 0, 0, 0};

/// The builder in `slot`, or an error once `finish()` spent it or outside the run that made it.
template <class Builder>
Builder& live(BuilderSlot<Builder>& slot) {
  if (currentRun().generation != slot.run) {
    throw py::value_error("a builder works only inside the run that made it");
  }
  if (!slot.builder) {
    throw py::value_error("the builder is already finished");
  }
  return *slot.builder;
}

/// Adds `primitive` to the builder `self` holds, read once the call's arguments have converted; a rejection is raised,
/// else `self` is returned, so calls chain.
template <class Builder, class Primitive>
py::object add(PyObject* self, Primitive&& primitive) {
  Builder& builder = live(unboxed<BuilderSlot<Builder>>(self));
  valueOf(withoutGil([&] { return primitive(builder); }));
  return py::reinterpret_borrow<py::object>(self);
}

template <class Builder>
py::object finish(PyObject* self) {
  auto& slot = unboxed<BuilderSlot<Builder>>(self);
  auto done = std::move(live(slot)).finish();
  slot.builder.reset();
  if constexpr (std::is_same_v<Builder, Annotations>) {
    // Paired with the image input `image_topic` names (`ScriptMedia::pairing`).
    std::tie(done.value.image_topic, done.value.timestamp) = currentRun().media.pairing(done.value.image_topic);
  }
  return newCharged(std::move(done));
}

py::object sceneSphere(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"center", "diameter", "color", nullptr};
  PyObject* center = nullptr;
  double diameter = 0.0;
  PyObject* color = nullptr;
  parse(args, kwargs, "Od|O:sphere", kNames, &center, &diameter, &color);
  const auto at = readPoint<sdk::Vector3, 3>(center, "'center'");
  const sdk::ColorRGBA paint = readColor(color, "color", kWhite);
  return add<Scene>(self, [&](Scene& builder) { return builder.addSphere(at, diameter, paint); });
}

py::object sceneCube(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"center", "size", "color", nullptr};
  PyObject* center = nullptr;
  PyObject* size = nullptr;
  PyObject* color = nullptr;
  parse(args, kwargs, "OO|O:cube", kNames, &center, &size, &color);
  const auto at = readPoint<sdk::Vector3, 3>(center, "'center'");
  const auto extent = readPoint<sdk::Vector3, 3>(size, "'size'");
  const sdk::ColorRGBA paint = readColor(color, "color", kWhite);
  return add<Scene>(self, [&](Scene& builder) { return builder.addCube(at, extent, paint); });
}

py::object sceneText(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"position", "text", "font_size", "color", "billboard", nullptr};
  PyObject* position = nullptr;
  const char* text = nullptr;
  double font_size = 0.0;
  PyObject* color = nullptr;
  int billboard = 0;
  parse(args, kwargs, "Osd|Op:text", kNames, &position, &text, &font_size, &color, &billboard);
  const auto at = readPoint<sdk::Vector3, 3>(position, "'position'");
  const sdk::ColorRGBA paint = readColor(color, "color", kWhite);
  return add<Scene>(self, [&](Scene& builder) { return builder.addText(at, text, font_size, paint, billboard != 0); });
}

py::object sceneLineStrip(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"points", "thickness", "color", nullptr};
  PyObject* points = nullptr;
  double thickness = 0.0;
  PyObject* color = nullptr;
  parse(args, kwargs, "Od|O:line_strip", kNames, &points, &thickness, &color);
  auto line = readPoints<sdk::Point3, 3>(points, "points");
  const sdk::ColorRGBA paint = readColor(color, "color", kWhite);
  return add<Scene>(self, [&](Scene& builder) {
    native::MemoryReservation(std::move(line.charge));  // handed over: the builder charges what it keeps
    return builder.addLineStrip(std::move(line.value), thickness, paint);
  });
}

py::object annotationsPoints(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"points", "type", "thickness", "outline_color", "fill_color", nullptr};
  PyObject* points = nullptr;
  const char* type = nullptr;
  PyObject* thickness = nullptr;
  PyObject* outline = nullptr;
  PyObject* fill = nullptr;
  parse(args, kwargs, "Os|OOO:points", kNames, &points, &type, &thickness, &outline, &fill);
  const sdk::PointsAnnotation defaults;
  auto drawn = readPoints<sdk::Point2, 2>(points, "points");
  const double width = numberOr(thickness, defaults.thickness);
  const sdk::ColorRGBA line = readColor(outline, "outline_color", defaults.color);
  const sdk::ColorRGBA area = readColor(fill, "fill_color", defaults.fill_color);
  return add<Annotations>(self, [&](Annotations& builder) {
    native::MemoryReservation(std::move(drawn.charge));  // handed over: the builder charges what it keeps
    return builder.addPoints(std::move(drawn.value), type, width, line, area);
  });
}

py::object annotationsCircle(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"center", "diameter", "thickness", "outline_color", "fill_color", nullptr};
  PyObject* center = nullptr;
  double diameter = 0.0;
  PyObject* thickness = nullptr;
  PyObject* outline = nullptr;
  PyObject* fill = nullptr;
  parse(args, kwargs, "Od|OOO:circle", kNames, &center, &diameter, &thickness, &outline, &fill);
  const sdk::CircleAnnotation defaults;
  const auto at = readPoint<sdk::Point2, 2>(center, "'center'");
  const double width = numberOr(thickness, defaults.thickness);
  const sdk::ColorRGBA line = readColor(outline, "outline_color", defaults.color);
  const sdk::ColorRGBA area = readColor(fill, "fill_color", defaults.fill_color);
  return add<Annotations>(
      self, [&](Annotations& builder) { return builder.addCircle(at, diameter, width, line, area); });
}

/// The SDK has no text background, so an opaque `background_color` is refused rather than dropped.
py::object annotationsText(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"position", "text", "font_size", "text_color", "background_color", nullptr};
  PyObject* position = nullptr;
  const char* text = nullptr;
  PyObject* font_size = nullptr;
  PyObject* color = nullptr;
  PyObject* background = nullptr;
  parse(args, kwargs, "Os|OOO:text", kNames, &position, &text, &font_size, &color, &background);
  const sdk::TextAnnotation defaults;
  const auto at = readPoint<sdk::Point2, 2>(position, "'position'");
  const double size = numberOr(font_size, defaults.font_size);
  const sdk::ColorRGBA paint = readColor(color, "text_color", defaults.color);
  if (readColor(background, "background_color", kTransparent).a != 0) {
    throw py::value_error("text: 'background_color' is not supported: the annotations have no text background");
  }
  return add<Annotations>(self, [&](Annotations& builder) { return builder.addText(at, text, size, paint); });
}

py::object sceneNew(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"frame_id", "id", nullptr};
  const char* frame_id = nullptr;
  const char* id = nullptr;
  parse(args, kwargs, "ss:new", kNames, &frame_id, &id);
  return newBoxed(
      g_scene_type, BuilderSlot<Scene>{currentRun().generation, valueOf(Scene::create(frame_id, id, limits().budget))});
}

py::object annotationsNew(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"image_topic", nullptr};
  const char* image_topic = nullptr;
  parse(args, kwargs, "s:new", kNames, &image_topic);
  return newBoxed(
      g_annotations_type,
      BuilderSlot<Annotations>{currentRun().generation, valueOf(Annotations::create(image_topic, limits().budget))});
}

// ---- pj.tf --------------------------------------------------------------------

/// `pj.tf.lookup(target, source, when=None)` -> `(transform, None)` or `(None, reason)`; see python_object_script.h.
py::object tfLookup(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"target", "source", "when", nullptr};
  const char* target = nullptr;
  const char* source = nullptr;
  PyObject* when = Py_None;
  parse(args, kwargs, "ss|O:lookup", kNames, &target, &source, &when);
  PythonRun& run = currentRun();
  const auto failure = [](const char* reason) { return newTuple(py::none(), newString(reason)); };
  Timestamp instant = run.options.query_time;
  pj::tf::Clock clock = pj::tf::Clock::kEntry;
  run.observed_tf = true;  // even with no snapshot: one appearing later changes the answer
  if (when == Py_None) {
    run.observed_query_clock = true;
  } else if (const sdk::BuiltinObject* object = toObject(when)) {
    const std::optional<Timestamp> stamp = ownStamp(*object);
    if (!stamp || *stamp == 0) {
      return failure("object has no stamp");
    }
    instant = *stamp;
    clock = pj::tf::Clock::kHeader;
  } else if (PyLong_Check(when) != 0) {
    int overflow = 0;
    instant = PyLong_AsLongLongAndOverflow(when, &overflow);
    if (overflow != 0) {
      throw py::value_error("pj.tf.lookup: time out of range");
    }
  } else {
    const double ns = numberOr(when, 0.0);
    if (!(std::abs(ns) < 9.2e18)) {
      throw py::value_error("pj.tf.lookup: time out of range");
    }
    instant = static_cast<Timestamp>(ns);
  }
  if (run.options.tf == nullptr) {
    return failure("no tf snapshot");
  }
  const std::string target_frame = target;
  const std::string source_frame = source;
  const auto found = withoutGil(
      [&] { return run.options.tf->tryLookupTransform(target_frame, source_frame, PJ::fromRaw(instant), clock); });
  if (!found) {
    return failure(tfLookupErrorText(found.error()));
  }
  return newTuple(
      newDict(
          {{"translation", vector(found->t.x, found->t.y, found->t.z)},
           {"rotation",
            newTuple(newFloat(found->q.x), newFloat(found->q.y), newFloat(found->q.z), newFloat(found->q.w))},
           {"target_frame", newString(target_frame)}}),
      py::none());
}

py::object tfFrames(PyObject* /*self*/) {
  PythonRun& run = currentRun();
  run.observed_tf = true;
  const std::vector<std::string> frames =
      run.options.tf != nullptr ? run.options.tf->getAllFrames() : std::vector<std::string>{};
  py::object out = checked(PyList_New(static_cast<Py_ssize_t>(frames.size())));
  for (std::size_t index = 0; index < frames.size(); ++index) {
    PyList_SET_ITEM(out.ptr(), static_cast<Py_ssize_t>(index), newString(frames[index]).release().ptr());
  }
  return out;
}

/// Read through a property, so every read is observed when it happens.
PyObject* tfNow(PyObject* /*self*/, void* /*closure*/) {
  return slotCall([&]() -> PyObject* {
    PythonRun& run = currentRun();
    run.observed_query_clock = true;
    return checked(PyLong_FromLongLong(run.options.query_time)).release().ptr();
  });
}

// ---- registration -------------------------------------------------------------

/// A plain type over `Value`: no constructor, no subclass, no attribute, no buffer; `slots` add its behaviour.
template <class Value>
PyTypeObject* plainType(const char* name, std::initializer_list<PyType_Slot> slots) {
  std::vector<PyType_Slot> all(slots);
  all.push_back({Py_tp_dealloc, reinterpret_cast<void*>(&destroyBoxed<Value>)});
  all.push_back({0, nullptr});
  PyType_Spec spec{
      name, static_cast<int>(sizeof(Boxed<Value>)), 0,
      Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_IMMUTABLETYPE, all.data()};
  return reinterpret_cast<PyTypeObject*>(checked(PyType_FromSpec(&spec)).release().ptr());
}

constexpr int kKeywords = METH_VARARGS | METH_KEYWORDS;

PjNamespaces* registerTypes() {
  static PyMethodDef object_methods[] = {
      {"count", cfunction(&withoutArguments<&count>), METH_NOARGS, nullptr},
      {"bounds", cfunction(&withoutArguments<&bounds>), METH_NOARGS, nullptr},
      {"extreme_point", cfunction(&withArguments<&extremePoint>), kKeywords, nullptr},
      {"crop_box", cfunction(&withArguments<&cropBox>), kKeywords, nullptr},
      {"filter", cfunction(&withArguments<&filter>), kKeywords, nullptr},
      {"transform", cfunction(&withArguments<&transform>), kKeywords, nullptr},
      {"xyz_points", cfunction(&withoutArguments<&xyzPoints>), METH_NOARGS, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef scene_methods[] = {
      {"sphere", cfunction(&withArguments<&sceneSphere>), kKeywords, nullptr},
      {"cube", cfunction(&withArguments<&sceneCube>), kKeywords, nullptr},
      {"text", cfunction(&withArguments<&sceneText>), kKeywords, nullptr},
      {"line_strip", cfunction(&withArguments<&sceneLineStrip>), kKeywords, nullptr},
      {"finish", cfunction(&withoutArguments<&finish<Scene>>), METH_NOARGS, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef annotations_methods[] = {
      {"points", cfunction(&withArguments<&annotationsPoints>), kKeywords, nullptr},
      {"circle", cfunction(&withArguments<&annotationsCircle>), kKeywords, nullptr},
      {"text", cfunction(&withArguments<&annotationsText>), kKeywords, nullptr},
      {"finish", cfunction(&withoutArguments<&finish<Annotations>>), METH_NOARGS, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef scene_new[] = {
      {"new", cfunction(&withArguments<&sceneNew>), kKeywords, nullptr}, {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef annotations_new[] = {
      {"new", cfunction(&withArguments<&annotationsNew>), kKeywords, nullptr}, {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef tf_methods[] = {
      {"lookup", cfunction(&withArguments<&tfLookup>), kKeywords, nullptr},
      {"frames", cfunction(&withoutArguments<&tfFrames>), METH_NOARGS, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyGetSetDef tf_properties[] = {
      {"now", &tfNow, nullptr, nullptr, nullptr}, {nullptr, nullptr, nullptr, nullptr, nullptr}};

  g_object_type = plainType<StructView>(
      "pj_binder.Object",
      {{Py_tp_getattro, reinterpret_cast<void*>(&attribute<true>)}, {Py_tp_methods, object_methods}});
  g_struct_type =
      plainType<StructView>("pj_binder.StructView", {{Py_tp_getattro, reinterpret_cast<void*>(&attribute<false>)}});
  g_list_type = plainType<ListView>(
      "pj_binder.ListView",
      {{Py_sq_length, reinterpret_cast<void*>(&listLength)}, {Py_sq_item, reinterpret_cast<void*>(&listItem)}});
  g_points_type = plainType<PointIterator>(
      "pj_binder.PointIterator", {{Py_tp_iter, reinterpret_cast<void*>(&PyObject_SelfIter)},
                                  {Py_tp_iternext, reinterpret_cast<void*>(&nextPoint)}});
  g_scene_type = plainType<BuilderSlot<Scene>>("pj_binder.SceneBuilder", {{Py_tp_methods, scene_methods}});
  g_annotations_type =
      plainType<BuilderSlot<Annotations>>("pj_binder.AnnotationsBuilder", {{Py_tp_methods, annotations_methods}});
  PyTypeObject* scene_namespace = plainType<Unit>("pj_binder.SceneNamespace", {{Py_tp_methods, scene_new}});
  PyTypeObject* annotations_namespace =
      plainType<Unit>("pj_binder.AnnotationsNamespace", {{Py_tp_methods, annotations_new}});
  PyTypeObject* tf = plainType<Unit>("pj_binder.Tf", {{Py_tp_methods, tf_methods}, {Py_tp_getset, tf_properties}});
  return new PjNamespaces{
      newBoxed(scene_namespace, Unit{}).release().ptr(), newBoxed(annotations_namespace, Unit{}).release().ptr(),
      newBoxed(tf, Unit{}).release().ptr()};
}

PjNamespaces* g_namespaces = nullptr;
std::atomic<std::uint64_t> g_runs{0};

}  // namespace

py::object checked(PyObject* object) {
  if (object == nullptr) {
    throw py::error_already_set();
  }
  return py::reinterpret_steal<py::object>(object);
}

py::object newString(std::string_view text) {
  return checked(PyUnicode_FromStringAndSize(text.data(), static_cast<Py_ssize_t>(text.size())));
}

PythonRun::PythonRun(const RunOptions& run_options, ScriptMedia& run_media)
    : options(run_options), media(run_media), generation(++g_runs), previous_(std::exchange(t_run, this)) {}

PythonRun::~PythonRun() {
  t_run = previous_;
}

const PjNamespaces& pjNamespaces() {
  if (g_namespaces == nullptr) {
    // Building may run a finalizer's bytecode, handing the GIL to another first caller: wait for it without the GIL.
    static std::once_flag once;
    py::gil_scoped_release no_gil;
    std::call_once(once, [] {
      py::gil_scoped_acquire gil;
      g_namespaces = registerTypes();
    });
  }
  return *g_namespaces;
}

py::object newObject(sdk::BuiltinObject object, std::shared_ptr<const void> keepalive) {
  return wrapObject(std::move(object), nullptr, std::move(keepalive));
}

const sdk::BuiltinObject* toObject(PyObject* value) {
  return g_object_type != nullptr && Py_TYPE(value) == g_object_type ? &heldObject(value) : nullptr;
}

std::shared_ptr<const void> anchorOf(PyObject* value) {
  return g_object_type != nullptr && Py_TYPE(value) == g_object_type ? unboxed<StructView>(value).keep : nullptr;
}

}  // namespace PJ::scripting::detail
