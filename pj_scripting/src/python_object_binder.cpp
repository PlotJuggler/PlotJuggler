// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "python_object_binder.h"

#include <algorithm>
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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "native/depth_ops.h"
#include "native/image_annotations_ops.h"
#include "native/image_ops.h"
#include "native/point_cloud_builder.h"
#include "native/scene_entities_ops.h"
#include "native/voxel_grid_ops.h"
#include "pj_base/builtin/field_table_registry.hpp"
#include "pj_base/builtin/video_frame.hpp"
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
using Cloud = native::PointCloudBuilder;
using Voxels = native::VoxelGridBuilder;

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
PyTypeObject* g_cloud_type = nullptr;
PyTypeObject* g_voxels_type = nullptr;
PyTypeObject* g_decoded_type = nullptr;

/// A C++ value inside a Python object of a plain type.
template <class Value>
struct Boxed {
  PyObject_HEAD Value value;
};

/// Freed struct-view blocks, reused by the next ones: a list walk frees and allocates one per element, and the
/// allocator round trip is about a third of a list-element read. Only a view freed inside a run is kept; a kept block
/// stays charged to the allocation budget of the run that allocated it and keeps that budget alive, so every run
/// empties the list at its end (`releaseRecycledViews`), and a block holds no value while kept. Guarded by the GIL.
constexpr std::size_t kMaxRecycledViews = 64;
std::array<PyObject*, kMaxRecycledViews> g_recycled_views{};
std::size_t g_recycled_count = 0;

template <class Value>
py::object newBoxed(PyTypeObject* type, Value value) {
  Boxed<Value>* self = nullptr;
  if constexpr (std::is_same_v<Value, StructView>) {
    if (type == g_struct_type && g_recycled_count > 0) {
      self = reinterpret_cast<Boxed<Value>*>(PyObject_Init(g_recycled_views[--g_recycled_count], type));
    }
  }
  if (self == nullptr) {
    self = PyObject_New(Boxed<Value>, type);
  }
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
  bool recycled = false;
  if constexpr (std::is_same_v<Value, StructView>) {
    recycled = type == g_struct_type && t_run != nullptr && g_recycled_count < kMaxRecycledViews;
    if (recycled) {
      g_recycled_views[g_recycled_count++] = self;
    }
  }
  if (!recycled) {
    PyObject_Free(self);
  }
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
    if (view.table == nullptr) {  // a type the SDK does not describe: its methods, no readable fields
      PyObject* method = PyObject_GenericGetAttr(self, name);
      if (method == nullptr && PyErr_ExceptionMatches(PyExc_AttributeError) != 0) {
        PyErr_Clear();
        throw py::attribute_error("a " + std::string(sdk::name(view.keep->object.type())) + " has no readable fields");
      }
      return method;
    }
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

// ---- images -------------------------------------------------------------------

py::object unavailable(std::string_view reason) {
  return checked(PyObject_CallOneArg(currentRun().unavailable, newString(reason).ptr()));
}

/// A pixel coordinate or size: a non-negative integer.
uint32_t pixelInteger(PyObject* value) {
  int overflow = 0;
  const long long number = PyLong_Check(value) != 0 ? PyLong_AsLongLongAndOverflow(value, &overflow) : -1;
  if (overflow != 0 || number < 0 || number > UINT32_MAX) {
    throw py::value_error("expected a non-negative pixel integer");
  }
  return static_cast<uint32_t>(number);
}

/// The receiver of an image method: exactly one of `image` and `depth` is set.
struct ImageReceiver {
  const sdk::Image* image = nullptr;
  const sdk::DepthImage* depth = nullptr;
};

ImageReceiver imageOf(PyObject* self) {
  const sdk::BuiltinObject& object = heldObject(self);
  ImageReceiver receiver{object.get<sdk::Image>(), object.get<sdk::DepthImage>()};
  if (receiver.image == nullptr && receiver.depth == nullptr) {
    throw py::type_error("image or depth image expected");
  }
  return receiver;
}

/// The receiver's pixels, decoded once per run.
Expected<DecodedPixels> decodeReceiver(ScriptMedia& media, const ImageReceiver& receiver) {
  if (receiver.image != nullptr) {
    return released([&] { return media.decode(receiver.image, *receiver.image); });
  }
  return released([&] { return media.decode(receiver.depth, native::depthAsImage(*receiver.depth)); });
}

py::object representation(PyObject* self) {
  const ImageReceiver receiver = imageOf(self);
  return newString(
      receiver.image != nullptr ? native::imageRepresentation(*receiver.image)
                                : native::imageRepresentation(*receiver.depth));
}

/// `(pixels, None)`, or `(None, reason)`.
py::object decode(PyObject* self) {
  Expected<DecodedPixels> pixels = decodeReceiver(currentRun().media, imageOf(self));
  if (!pixels) {
    return newTuple(py::none(), newString(pixels.error()));
  }
  return newTuple(newBoxed(g_decoded_type, std::move(*pixels)), py::none());
}

/// A cloud, or `pj.unavailable(reason)`.
py::object toPointCloud(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"camera_info", "step", "max_depth", nullptr};
  PyObject* camera_object = nullptr;
  PyObject* step = Py_None;
  PyObject* max_depth = Py_None;
  parse(args, kwargs, "O|OO:to_point_cloud", kNames, &camera_object, &step, &max_depth);
  const sdk::BuiltinObject* object = toObject(camera_object);
  const sdk::CameraInfo* camera = object != nullptr ? object->get<sdk::CameraInfo>() : nullptr;
  if (camera == nullptr) {
    throw py::type_error("to_point_cloud: camera info expected");
  }
  native::DepthOptions options;
  if (step != Py_None) {
    options.step = pixelInteger(step);
  }
  options.max_depth = numberOr(max_depth, options.max_depth);
  const ImageReceiver receiver = imageOf(self);
  if (receiver.depth != nullptr && !receiver.depth->distortion_model.empty()) {
    return unavailable("depth must be rectified before unprojection");
  }
  ScriptMedia& media = currentRun().media;
  Expected<DecodedPixels> pixels = decodeReceiver(media, receiver);
  if (!pixels) {
    return unavailable(pixels.error());
  }
  const std::string& encoding = receiver.image != nullptr ? receiver.image->encoding : receiver.depth->encoding;
  const Timestamp stamp = ownStamp(heldObject(self)).value_or(0);
  Expected<sdk::PointCloud> cloud =
      released([&] { return native::depthToPointCloud(*pixels, encoding, stamp, *camera, options, media.limits()); });
  if (!cloud && cloud.error() != native::kCancelledError) {
    return unavailable(cloud.error());
  }
  return newCloud(std::move(cloud));
}

py::object pixel(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"x", "y", nullptr};
  PyObject* x = nullptr;
  PyObject* y = nullptr;
  parse(args, kwargs, "OO:pixel", kNames, &x, &y);
  const std::vector<float> samples =
      valueOf(native::imagePixel(unboxed<DecodedPixels>(self), pixelInteger(x), pixelInteger(y)));
  if (samples.size() == 1) {
    return newFloat(samples[0]);
  }
  constexpr const char* kChannels[] = {"r", "g", "b", "a"};
  py::object out = checked(PyDict_New());
  for (std::size_t channel = 0; channel < samples.size(); ++channel) {
    if (PyDict_SetItemString(out.ptr(), kChannels[channel], newFloat(samples[channel]).ptr()) != 0) {
      throw py::error_already_set();
    }
  }
  return out;
}

py::object toGray(PyObject* self) {
  const DecodedPixels& image = unboxed<DecodedPixels>(self);
  return newBoxed(g_decoded_type, valueOf(released([&] { return native::imageToGray(image, limits()); })));
}

py::object crop(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"x", "y", "width", "height", nullptr};
  std::array<PyObject*, 4> values{};
  parse(args, kwargs, "OOOO:crop", kNames, &values[0], &values[1], &values[2], &values[3]);
  const uint32_t left = pixelInteger(values[0]);
  const uint32_t top = pixelInteger(values[1]);
  const uint32_t columns = pixelInteger(values[2]);
  const uint32_t rows = pixelInteger(values[3]);
  const DecodedPixels& image = unboxed<DecodedPixels>(self);
  return newBoxed(
      g_decoded_type, valueOf(released([&] { return native::imageCrop(image, left, top, columns, rows, limits()); })));
}

/// `threshold(op, value, channel=0)`: channel 0-3 or "r", "g", "b", "a"; matches over the budget are unavailable.
py::object threshold(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"op", "value", "channel", nullptr};
  const char* op = nullptr;
  double value = 0.0;
  PyObject* channel_value = nullptr;
  parse(args, kwargs, "sd|O:threshold", kNames, &op, &value, &channel_value);
  const native::CompareOp compare = comparison(op, "threshold");
  uint32_t channel = 0;
  if (channel_value != nullptr && PyUnicode_Check(channel_value) != 0) {
    Py_ssize_t length = 0;
    const char* text = PyUnicode_AsUTF8AndSize(channel_value, &length);
    if (text == nullptr) {
      throw py::error_already_set();
    }
    channel = valueOf(thresholdChannel(std::string_view(text, static_cast<std::size_t>(length))));
  } else if (channel_value != nullptr) {
    channel = pixelInteger(channel_value);
  }
  const DecodedPixels& image = unboxed<DecodedPixels>(self);
  auto annotations = released([&] { return native::imageThreshold(image, compare, value, channel, limits()); });
  if (!annotations && annotations.error().starts_with(native::kNativeLimitError)) {
    return unavailable(annotations.error());
  }
  return newCharged(valueOf(std::move(annotations)));
}

py::object stats(PyObject* self) {
  const DecodedPixels& image = unboxed<DecodedPixels>(self);
  const native::ImageStats found = valueOf(released([&] { return native::imageStats(image, limits()); }));
  return newDict({{"min", newFloat(found.min)}, {"max", newFloat(found.max)}, {"mean", newFloat(found.mean)}});
}

/// `width`, `height` or `channels` (the member `closure` points to) of decoded pixels.
PyObject* decodedSize(PyObject* self, void* closure) {
  const auto member = reinterpret_cast<std::uintptr_t>(closure);
  const DecodedPixels& image = unboxed<DecodedPixels>(self);
  return PyLong_FromUnsignedLong(member == 0 ? image.width : member == 1 ? image.height : image.channels);
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
  } else if constexpr (requires { done.value.timestamp_ns; }) {
    currentRun().media.stampUnset(done.value.timestamp_ns);
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

// ---- video --------------------------------------------------------------------

/// `(frame, None)`, the frame presented at `t` (a whole number of nanoseconds, an int or a float, on the dataset clock;
/// not a bool) as a dict of `image` and `presentation_ns`, or `(None, reason)`.
py::object frameAt(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"t", nullptr};
  PyObject* when = nullptr;
  parse(args, kwargs, "O:frame_at", kNames, &when);
  const auto refuse = [] { return py::value_error("frame_at: time must be a whole number of nanoseconds"); };
  Timestamp t = 0;
  if (PyBool_Check(when) != 0) {
    throw refuse();
  }
  if (PyLong_Check(when) != 0) {
    int overflow = 0;
    t = PyLong_AsLongLongAndOverflow(when, &overflow);
    if (overflow != 0) {
      throw refuse();
    }
  } else if (PyFloat_Check(when) != 0) {
    const double ns = PyFloat_AS_DOUBLE(when);
    if (!(std::abs(ns) < 9.2e18) || std::floor(ns) != ns) {
      throw refuse();
    }
    t = static_cast<Timestamp>(ns);
  } else {
    throw refuse();
  }
  const sdk::VideoFrame* video = heldObject(self).get<sdk::VideoFrame>();
  if (video == nullptr) {
    throw py::type_error("frame_at: video frame expected");
  }
  ScriptMedia& media = currentRun().media;
  Expected<sdk::BuiltinObject> frame = released([&] { return media.frameAt(video, t); });
  if (!frame) {
    return newTuple(py::none(), newString(frame.error()));
  }
  const Timestamp presentation = frame->get<sdk::Image>()->timestamp_ns;
  return newTuple(
      newDict(
          {{"image", newObject(std::move(*frame))}, {"presentation_ns", checked(PyLong_FromLongLong(presentation))}}),
      py::none());
}

// ---- pj.cloud, pj.voxels and cloud.downsample ---------------------------------------

/// One past the widest preset's values, so an extra value still reaches the builder's arity check.
constexpr std::size_t kMaxPointValues = 7;
using PointValues = std::array<double, kMaxPointValues>;

/// The numbers of the tuple `items`, at most `kMaxPointValues`, into `values`; their count. `subject` names them in the
/// error.
std::size_t readValues(PyObject* items, PointValues& values, const char* subject) {
  const auto count = std::min(static_cast<std::size_t>(PyTuple_GET_SIZE(items)), kMaxPointValues);
  for (std::size_t index = 0; index < count; ++index) {
    values[index] = PyFloat_AsDouble(PyTuple_GET_ITEM(items, static_cast<Py_ssize_t>(index)));
    if (values[index] == -1.0 && PyErr_Occurred() != nullptr) {
      PyErr_Clear();
      throw py::value_error(std::string(subject) + " must be numbers");
    }
  }
  return count;
}

/// An int argument, clamped to +-2^62 (outside any grid); anything else raises `error`. Reads no script code.
int64_t integerOf(PyObject* value, const char* error) {
  if (PyLong_Check(value) == 0) {
    throw py::type_error(error);
  }
  constexpr long long kLimit = 1LL << 62;
  int overflow = 0;
  const long long number = PyLong_AsLongLongAndOverflow(value, &overflow);
  if (overflow != 0) {
    return overflow > 0 ? kLimit : -kLimit;
  }
  return std::clamp(number, -kLimit, kLimit);
}

py::object cloudNew(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"preset", "frame_id", nullptr};
  const char* preset = nullptr;
  const char* frame_id = nullptr;
  parse(args, kwargs, "ss:new", kNames, &preset, &frame_id);
  const std::optional<native::CloudPreset> parsed = native::parseCloudPreset(preset);
  if (!parsed) {
    throw py::value_error("'preset' must be \"xyz\", \"xyzi\" or \"xyzrgb\"");
  }
  return newBoxed(
      g_cloud_type,
      BuilderSlot<Cloud>{currentRun().generation, valueOf(Cloud::create(*parsed, frame_id, limits().budget))});
}

/// `add(x, y, z[, intensity | r, g, b])`: one point, O(1), with the GIL.
py::object cloudAdd(PyObject* self, PyObject* args, PyObject* kwargs) {
  if (kwargs != nullptr && PyDict_GET_SIZE(kwargs) != 0) {
    throw py::type_error("add: the point's values are positional");
  }
  PointValues values{};
  const std::size_t count = readValues(args, values, "add: the point's values");
  valueOf(live(unboxed<BuilderSlot<Cloud>>(self)).add(std::span<const double>(values.data(), count)));
  return py::reinterpret_borrow<py::object>(self);
}

/// `add_points(points)`: every entry converts first, into a copy charged before it is allocated, then the builder takes
/// them all without the GIL.
py::object cloudAddPoints(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"points", nullptr};
  PyObject* points = nullptr;
  parse(args, kwargs, "O:add_points", kNames, &points);
  auto& slot = unboxed<BuilderSlot<Cloud>>(self);
  const py::object items = sequenceOf(points, "'points'");
  const auto count = static_cast<std::size_t>(PyTuple_GET_SIZE(items.ptr()));
  const std::size_t arity = live(slot).arity();
  native::MemoryReservation charge(limits().budget);
  valueOf(charge.grow(count * arity * sizeof(double)));
  std::vector<double> values;
  values.reserve(count * arity);
  PointValues point{};
  for (std::size_t index = 0; index < count; ++index) {
    const py::object entry =
        sequenceOf(PyTuple_GET_ITEM(items.ptr(), static_cast<Py_ssize_t>(index)), "'points' entry");
    const std::size_t size = readValues(entry.ptr(), point, "'points' entries");
    valueOf(live(slot).acceptsArity(size));
    values.insert(values.end(), point.begin(), point.begin() + static_cast<std::ptrdiff_t>(size));
  }
  Cloud& builder = live(slot);
  valueOf(withoutGil([&]() -> Status {
    if (Status reserved = builder.reserve(count); !reserved) {
      return reserved;
    }
    for (std::size_t index = 0; index < count; ++index) {
      if (Status added = builder.add(std::span<const double>(values.data() + index * arity, arity)); !added) {
        return added;
      }
    }
    return okStatus();
  }));
  return py::reinterpret_borrow<py::object>(self);
}

py::object voxelsNew(PyObject* /*self*/, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"frame_id", "origin", "cell_size", "dims", nullptr};
  constexpr const char* kBadDims = "'dims' must be 3 integers greater than 0";
  const char* frame_id = nullptr;
  PyObject* origin = nullptr;
  double cell_size = 0.0;
  PyObject* dims = nullptr;
  parse(args, kwargs, "sOdO:new", kNames, &frame_id, &origin, &cell_size, &dims);
  const auto corner = readPoint<sdk::Vector3, 3>(origin, "'origin'");
  const py::object counts = sequenceOf(dims, "'dims'");
  if (PyTuple_GET_SIZE(counts.ptr()) != 3) {
    throw py::value_error(kBadDims);
  }
  std::array<int64_t, 3> cells{};
  for (std::size_t axis = 0; axis < cells.size(); ++axis) {
    cells[axis] = integerOf(PyTuple_GET_ITEM(counts.ptr(), static_cast<Py_ssize_t>(axis)), kBadDims);
  }
  return newBoxed(
      g_voxels_type,
      BuilderSlot<Voxels>{
          currentRun().generation, valueOf(Voxels::create(frame_id, corner, cell_size, cells, limits().budget))});
}

/// `set(i, j, k, value)`, the indices 0-based.
py::object voxelsSet(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"i", "j", "k", "value", nullptr};
  constexpr const char* kNotIntegers = "set: i, j and k must be integers";
  PyObject* i = nullptr;
  PyObject* j = nullptr;
  PyObject* k = nullptr;
  double value = 0.0;
  parse(args, kwargs, "OOOd:set", kNames, &i, &j, &k, &value);
  const int64_t column = integerOf(i, kNotIntegers);
  const int64_t row = integerOf(j, kNotIntegers);
  const int64_t slice = integerOf(k, kNotIntegers);
  valueOf(live(unboxed<BuilderSlot<Voxels>>(self)).set(column, row, slice, value));
  return py::reinterpret_borrow<py::object>(self);
}

py::object cloudDownsample(PyObject* self, PyObject* args, PyObject* kwargs) {
  static constexpr const char* kNames[] = {"voxel_size", nullptr};
  double voxel_size = 0.0;
  parse(args, kwargs, "d:downsample", kNames, &voxel_size);
  const sdk::PointCloud& cloud = cloudOf(self);
  return newCharged(valueOf(released([&] { return native::downsample(cloud, voxel_size, limits()); })));
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
      {"downsample", cfunction(&withArguments<&cloudDownsample>), kKeywords, nullptr},
      {"filter", cfunction(&withArguments<&filter>), kKeywords, nullptr},
      {"transform", cfunction(&withArguments<&transform>), kKeywords, nullptr},
      {"xyz_points", cfunction(&withoutArguments<&xyzPoints>), METH_NOARGS, nullptr},
      {"representation", cfunction(&withoutArguments<&representation>), METH_NOARGS, nullptr},
      {"decode", cfunction(&withoutArguments<&decode>), METH_NOARGS, nullptr},
      {"to_point_cloud", cfunction(&withArguments<&toPointCloud>), kKeywords, nullptr},
      {"frame_at", cfunction(&withArguments<&frameAt>), kKeywords, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef decoded_methods[] = {
      {"pixel", cfunction(&withArguments<&pixel>), kKeywords, nullptr},
      {"to_gray", cfunction(&withoutArguments<&toGray>), METH_NOARGS, nullptr},
      {"crop", cfunction(&withArguments<&crop>), kKeywords, nullptr},
      {"threshold", cfunction(&withArguments<&threshold>), kKeywords, nullptr},
      {"stats", cfunction(&withoutArguments<&stats>), METH_NOARGS, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyGetSetDef decoded_properties[] = {
      {"width", &decodedSize, nullptr, nullptr, reinterpret_cast<void*>(std::uintptr_t{0})},
      {"height", &decodedSize, nullptr, nullptr, reinterpret_cast<void*>(std::uintptr_t{1})},
      {"channels", &decodedSize, nullptr, nullptr, reinterpret_cast<void*>(std::uintptr_t{2})},
      {nullptr, nullptr, nullptr, nullptr, nullptr}};
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
  static PyMethodDef cloud_methods[] = {
      {"add", cfunction(&withArguments<&cloudAdd>), kKeywords, nullptr},
      {"add_points", cfunction(&withArguments<&cloudAddPoints>), kKeywords, nullptr},
      {"finish", cfunction(&withoutArguments<&finish<Cloud>>), METH_NOARGS, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef voxels_methods[] = {
      {"set", cfunction(&withArguments<&voxelsSet>), kKeywords, nullptr},
      {"finish", cfunction(&withoutArguments<&finish<Voxels>>), METH_NOARGS, nullptr},
      {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef cloud_new[] = {
      {"new", cfunction(&withArguments<&cloudNew>), kKeywords, nullptr}, {nullptr, nullptr, 0, nullptr}};
  static PyMethodDef voxels_new[] = {
      {"new", cfunction(&withArguments<&voxelsNew>), kKeywords, nullptr}, {nullptr, nullptr, 0, nullptr}};
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
  g_decoded_type = plainType<DecodedPixels>(
      "pj_binder.DecodedImage", {{Py_tp_methods, decoded_methods}, {Py_tp_getset, decoded_properties}});
  g_scene_type = plainType<BuilderSlot<Scene>>("pj_binder.SceneBuilder", {{Py_tp_methods, scene_methods}});
  g_annotations_type =
      plainType<BuilderSlot<Annotations>>("pj_binder.AnnotationsBuilder", {{Py_tp_methods, annotations_methods}});
  g_cloud_type = plainType<BuilderSlot<Cloud>>("pj_binder.CloudBuilder", {{Py_tp_methods, cloud_methods}});
  g_voxels_type = plainType<BuilderSlot<Voxels>>("pj_binder.VoxelsBuilder", {{Py_tp_methods, voxels_methods}});
  PyTypeObject* scene_namespace = plainType<Unit>("pj_binder.SceneNamespace", {{Py_tp_methods, scene_new}});
  PyTypeObject* annotations_namespace =
      plainType<Unit>("pj_binder.AnnotationsNamespace", {{Py_tp_methods, annotations_new}});
  PyTypeObject* cloud_namespace = plainType<Unit>("pj_binder.CloudNamespace", {{Py_tp_methods, cloud_new}});
  PyTypeObject* voxels_namespace = plainType<Unit>("pj_binder.VoxelsNamespace", {{Py_tp_methods, voxels_new}});
  PyTypeObject* tf = plainType<Unit>("pj_binder.Tf", {{Py_tp_methods, tf_methods}, {Py_tp_getset, tf_properties}});
  return new PjNamespaces{
      newBoxed(scene_namespace, Unit{}).release().ptr(), newBoxed(annotations_namespace, Unit{}).release().ptr(),
      newBoxed(cloud_namespace, Unit{}).release().ptr(), newBoxed(voxels_namespace, Unit{}).release().ptr(),
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

void releaseRecycledViews() {
  while (g_recycled_count > 0) {
    PyObject_Free(g_recycled_views[--g_recycled_count]);
  }
}

std::size_t recycledViewCount() {
  return g_recycled_count;
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
