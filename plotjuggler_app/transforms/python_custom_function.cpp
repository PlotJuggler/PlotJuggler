#include "python_custom_function.h"

#include <QTextStream>
#include <QCoreApplication>
#include <QString>
#include <QDebug>

#include <atomic>

static std::once_flag g_py_once;
static std::atomic<bool> g_py_unavailable{ false };
static std::string g_py_unavailable_reason;

PythonCustomFunction::PythonCustomFunction(SnippetData snippet) : CustomFunction(snippet)
{
  initEngine();

  {
    QTextStream in(&snippet.global_vars);
    while (!in.atEnd())
    {
      in.readLine();
      global_lines_++;
    }
  }
  {
    QTextStream in(&snippet.function);
    while (!in.atEnd())
    {
      in.readLine();
      function_lines_++;
    }
  }
}

PythonCustomFunction::~PythonCustomFunction()
{
  std::unique_lock<std::mutex> lk(mutex_);

  // Release Python-owned objects under the GIL before destruction.
  if (_py_calc)
  {
    PyGILState_STATE gil = PyGILState_Ensure();
    Py_DECREF(_py_calc);
    _py_calc = nullptr;
    PyGILState_Release(gil);
  }
  if (_locals)
  {
    PyGILState_STATE gil = PyGILState_Ensure();
    Py_DECREF(_locals);
    _locals = nullptr;
    PyGILState_Release(gil);
  }
  if (_globals)
  {
    PyGILState_STATE gil = PyGILState_Ensure();
    Py_DECREF(_globals);
    _globals = nullptr;
    PyGILState_Release(gil);
  }
}

// Initialize the embedded Python runtime only once per process.
//
// Uses the PEP 587 PyConfig API so a broken Python install (missing stdlib,
// ABI mismatch on portable bundles like AppImage, etc.) returns a recoverable
// PyStatus instead of calling Py_FatalError() and aborting the process.
// On failure we set g_py_unavailable so subsequent calls throw a clean
// std::runtime_error that the UI already handles via QMessageBox::warning.
void PythonCustomFunction::ensurePythonInitialized()
{
  std::call_once(g_py_once, []() {
    if (Py_IsInitialized())
    {
      return;
    }

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    // Don't let CPython install signal handlers or parse argv.
    config.install_signal_handlers = 0;
    config.parse_argv = 0;
    config.isolated = 0;

    // On Windows, the installer ships the python.org "embeddable Python"
    // distribution flattened next to plotjuggler.exe (pythonXY.dll,
    // pythonXY.zip, pythonXY._pth, …). The presence of pythonXY._pth puts
    // CPython into isolated _pth mode, which derives sys.path entirely from
    // that file relative to the loaded DLL — overriding PYTHONHOME, the
    // registry, and any PyConfig.home we might set. So we deliberately
    // don't touch PyConfig.home here.

    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);

    if (PyStatus_Exception(status))
    {
      // Write the reason BEFORE the release-store on the flag so any thread
      // that observes the flag set is guaranteed (via acquire/release) to see
      // the published string. Reversing this order would race.
      g_py_unavailable_reason =
          status.err_msg ? status.err_msg : "Python interpreter failed to initialize";
      g_py_unavailable.store(true, std::memory_order_release);
      qWarning() << "Embedded Python disabled:" << QString::fromStdString(g_py_unavailable_reason);
      // Do NOT call Py_ExitStatusException — that aborts. Just leave Python down.
      return;
    }

    // Verify the bundled stdlib actually loads. PyConfig_InitPythonConfig
    // succeeding only proves the interpreter object is up; if lib-dynload
    // .so files (or the equivalent .pyd on Windows) link against host
    // libraries that don't exist on the target machine, `import` itself
    // will still fail. Probing `import math` here forces the failure now
    // rather than at first user-snippet execution.
    {
      PyObject* m = PyImport_ImportModule("math");
      if (!m)
      {
        PyErr_Clear();
        g_py_unavailable_reason = "embedded Python stdlib is incomplete (cannot import 'math')";
        g_py_unavailable.store(true, std::memory_order_release);
        qWarning() << "Embedded Python disabled:"
                   << QString::fromStdString(g_py_unavailable_reason);
        return;
      }
      Py_DECREF(m);
    }

    PyEval_SaveThread();
  });

  if (g_py_unavailable.load(std::memory_order_acquire))
  {
    throw std::runtime_error(
        "Python support is unavailable in this build: " + g_py_unavailable_reason +
        ". Use Lua for custom functions, or run a build of "
        "PlotJuggler that ships a working Python runtime.");
  }
}

bool PythonCustomFunction::probeAvailable()
{
  try
  {
    ensurePythonInitialized();
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}

bool PythonCustomFunction::isAvailable()
{
  return !g_py_unavailable.load(std::memory_order_acquire);
}

// Convert the current Python exception into a readable traceback string.
std::string PythonCustomFunction::fetchPythonExceptionWithTraceback()
{
  PyObject* ptype = nullptr;
  PyObject* pvalue = nullptr;
  PyObject* ptrace = nullptr;

  PyErr_Fetch(&ptype, &pvalue, &ptrace);
  PyErr_NormalizeException(&ptype, &pvalue, &ptrace);

  PyObject* tbmod = PyImport_ImportModule("traceback");
  PyObject* fmt = tbmod ? PyObject_GetAttrString(tbmod, "format_exception") : nullptr;

  std::string out = "Python error";

  if (fmt && PyCallable_Check(fmt))
  {
    PyObject* list =
        PyObject_CallFunctionObjArgs(fmt, ptype ? ptype : Py_None, pvalue ? pvalue : Py_None,
                                     ptrace ? ptrace : Py_None, nullptr);
    if (list)
    {
      PyObject* joined = PyUnicode_Join(PyUnicode_FromString(""), list);
      if (joined)
      {
        const char* s = PyUnicode_AsUTF8(joined);
        if (s)
        {
          out = s;
        }
        Py_DECREF(joined);
      }
      Py_DECREF(list);
    }
  }
  else
  {
    if (pvalue)
    {
      PyObject* s = PyObject_Str(pvalue);
      if (s)
      {
        const char* c = PyUnicode_AsUTF8(s);
        if (c)
        {
          out = c;
        }
        Py_DECREF(s);
      }
    }
  }

  Py_XDECREF(fmt);
  Py_XDECREF(tbmod);

  Py_XDECREF(ptype);
  Py_XDECREF(pvalue);
  Py_XDECREF(ptrace);

  return out;
}

// Map Python traceback locations to PlotJuggler-style global/function errors.
std::string PythonCustomFunction::formatError(const std::string& tb_text) const
{
  const bool is_function = (tb_text.find("<PJ_FUNCTION>") != std::string::npos);
  const char* tag = is_function ? "[Function]: line " : "[Global]: line ";

  auto pos = tb_text.find(is_function ? "<PJ_FUNCTION>" : "<PJ_GLOBAL>");
  if (pos == std::string::npos)
  {
    return tb_text;
  }

  auto line_pos = tb_text.find("line ", pos);
  if (line_pos == std::string::npos)
  {
    return tb_text;
  }
  line_pos += 5;

  int line_num = 0;
  while (line_pos < tb_text.size() && std::isdigit(tb_text[line_pos]))
  {
    line_num = line_num * 10 + (tb_text[line_pos] - '0');
    line_pos++;
  }

  if (is_function)
  {
    line_num -= 1;
    if (line_num < 1)
    {
      line_num = 1;
    }
  }

  // Use the last traceback line as the final user-facing error message.
  auto last_nl = tb_text.find_last_of('\n');
  std::string last_line = (last_nl == std::string::npos) ? tb_text : tb_text.substr(last_nl + 1);
  if (last_line.empty())
  {
    last_line = tb_text;
  }

  std::string out = tag + std::to_string(line_num) + ": " + last_line;
  return out;
}

static std::string validatePythonImports(const QString& code, const char* tag)
{
  QTextStream in(const_cast<QString*>(&code), QIODevice::ReadOnly);

  int line_no = 0;
  while (!in.atEnd())
  {
    QString line = in.readLine();
    line_no++;

    QString trimmed = line.trimmed();

    if (trimmed.startsWith("import "))
    {
      if (trimmed != "import math")
      {
        return QString("%1: line %2: only 'import math' is allowed")
            .arg(tag)
            .arg(line_no)
            .toStdString();
      }
    }

    if (trimmed.startsWith("from "))
    {
      return QString("%1: line %2: 'from ... import ...' is not allowed")
          .arg(tag)
          .arg(line_no)
          .toStdString();
    }
  }

  return "";
}

void PythonCustomFunction::initEngine()
{
  std::unique_lock<std::mutex> lk(mutex_);

  // Ensure the embedded Python interpreter is ready before rebuilding the engine state.
  ensurePythonInitialized();
  PyGILState_STATE gil = PyGILState_Ensure();

  // Reset any previously compiled Python state before reinitialization.
  if (_py_calc)
  {
    Py_DECREF(_py_calc);
    _py_calc = nullptr;
  }
  if (_locals)
  {
    Py_DECREF(_locals);
    _locals = nullptr;
  }
  if (_globals)
  {
    Py_DECREF(_globals);
    _globals = nullptr;
  }

  _globals = PyDict_New();
  PyDict_SetItemString(_globals, "__builtins__", PyEval_GetBuiltins());

#ifdef PJ_HAS_NANOBIND
  {
    QString app_dir = QCoreApplication::applicationDirPath();
    QString py_cmd = QString("import sys\nsys.path.insert(0, r'%1')\n").arg(app_dir);
    PyRun_SimpleString(py_cmd.toStdString().c_str());

    PyObject* pj_module = PyImport_ImportModule("pj");
    if (!pj_module)
    {
      std::string tb = fetchPythonExceptionWithTraceback();
      PyGILState_Release(gil);
      throw std::runtime_error("Failed to import pj: " + tb);
    }

    PyDict_SetItemString(_globals, "pj", pj_module);
    Py_DECREF(pj_module);
  }
#endif

  _locals = _globals;
  Py_INCREF(_locals);

  // Restrict imports to keep the execution environment minimal and predictable.
  std::string err = validatePythonImports(_snippet.global_vars, "[Global]");
  if (!err.empty())
  {
    PyGILState_Release(gil);
    throw std::runtime_error(err);
  }

  // Execute the user-defined global code before compiling calc(...).
  const std::string global_code = _snippet.global_vars.toStdString();
  if (!global_code.empty())
  {
    PyObject* r = PyRun_StringFlags(global_code.c_str(), Py_file_input, _globals, _locals, nullptr);
    if (!r)
    {
      std::string tb = fetchPythonExceptionWithTraceback();
      PyGILState_Release(gil);
      throw std::runtime_error(formatError(tb));
    }
    Py_DECREF(r);
  }

  err = validatePythonImports(_snippet.function, "[Function]");
  if (!err.empty())
  {
    PyGILState_Release(gil);
    throw std::runtime_error(err);
  }

  // Wrap the user snippet inside calc(time, value, v1, ..., vN).
  std::string def = "def calc(time, value";
  for (int i = 0; i < (int)_snippet.additional_sources.size(); i++)
  {
    if (_snippet.additional_sources[i] != _snippet.linked_source)
    {
      def += ", v" + std::to_string(i + 1);
    }
  }
  def += "):\n";

  const std::string body = _snippet.function.toStdString();
  if (body.empty())
  {
    def += "    return float('nan')\n";
  }
  else
  {
    size_t start = 0;
    while (start < body.size())
    {
      size_t end = body.find('\n', start);
      if (end == std::string::npos)
      {
        end = body.size();
      }
      std::string line = body.substr(start, end - start);
      def += "    " + line + "\n";
      start = end + 1;
    }
  }

  // Compile with a synthetic filename so traceback parsing remains predictable.
  PyObject* compiled = Py_CompileString(def.c_str(), "<PJ_FUNCTION>", Py_file_input);
  if (!compiled)
  {
    std::string tb = fetchPythonExceptionWithTraceback();
    PyGILState_Release(gil);
    throw std::runtime_error(formatError(tb));
  }

  PyObject* execres = PyEval_EvalCode(compiled, _globals, _locals);
  Py_DECREF(compiled);

  if (!execres)
  {
    std::string tb = fetchPythonExceptionWithTraceback();
    PyGILState_Release(gil);
    throw std::runtime_error(formatError(tb));
  }
  Py_DECREF(execres);

  PyObject* fn = PyDict_GetItemString(_locals, "calc");
  if (!fn)
  {
    fn = PyDict_GetItemString(_globals, "calc");
  }
  if (!fn || !PyCallable_Check(fn))
  {
    PyGILState_Release(gil);
    throw std::runtime_error("Python Engine: calc is not callable");
  }

  Py_INCREF(fn);
  _py_calc = fn;

  PyGILState_Release(gil);
}

void PythonCustomFunction::parsePythonResult(PyObject* result, double time,
                                             std::vector<PlotData::Point>& points,
                                             PyGILState_STATE gil)
{
  if (!result)
  {
    std::string tb = fetchPythonExceptionWithTraceback();
    PyGILState_Release(gil);
    throw std::runtime_error(formatError(tb));
  }

  points.clear();

  if (PyTuple_Check(result) && PyTuple_Size(result) == 2)
  {
    PlotData::Point p;
    p.x = PyFloat_AsDouble(PyTuple_GetItem(result, 0));
    p.y = PyFloat_AsDouble(PyTuple_GetItem(result, 1));
    points.push_back(p);
    Py_DECREF(result);
    PyGILState_Release(gil);
    return;
  }

  if (PyFloat_Check(result) || PyLong_Check(result))
  {
    PlotData::Point p;
    p.x = time;
    p.y = PyFloat_AsDouble(result);
    points.push_back(p);
    Py_DECREF(result);
    PyGILState_Release(gil);
    return;
  }

  if (PyList_Check(result) || PyTuple_Check(result))
  {
    const Py_ssize_t len = PySequence_Size(result);
    for (Py_ssize_t i = 0; i < len; i++)
    {
      PyObject* item = PySequence_GetItem(result, i);
      if (!item)
      {
        Py_DECREF(result);
        std::string tb = fetchPythonExceptionWithTraceback();
        PyGILState_Release(gil);
        throw std::runtime_error(formatError(tb));
      }
      if (!(PyTuple_Check(item) && PyTuple_Size(item) == 2) &&
          !(PyList_Check(item) && PyList_Size(item) == 2))
      {
        Py_DECREF(item);
        Py_DECREF(result);
        PyGILState_Release(gil);
        throw std::runtime_error("Wrong return object: expecting either a single value, "
                                 "two values (time, value) "
                                 "or an array of two-sized arrays (time, value)");
      }
      PyObject* rx = PySequence_GetItem(item, 0);
      PyObject* ry = PySequence_GetItem(item, 1);
      PlotData::Point p;
      p.x = PyFloat_AsDouble(rx);
      p.y = PyFloat_AsDouble(ry);
      Py_DECREF(rx);
      Py_DECREF(ry);
      Py_DECREF(item);
      points.push_back(p);
    }
    Py_DECREF(result);
    PyGILState_Release(gil);
    return;
  }

  Py_DECREF(result);
  PyGILState_Release(gil);
  throw std::runtime_error("Wrong return object: expecting either a single value, "
                           "two values (time, value) "
                           "or an array of two-sized arrays (time, value)");
}

void PythonCustomFunction::calculatePoints(const MixedSource& main_src,
                                           const std::vector<MixedSource>& additional_src,
                                           size_t point_index, std::vector<PlotData::Point>& points)
{
  std::unique_lock<std::mutex> lk(mutex_);

  if ((int)additional_src.size() > 8)
  {
    throw std::runtime_error("Python Engine: maximum number of additional data sources is 8");
  }

  PyGILState_STATE gil = PyGILState_Ensure();

  if (!_py_calc)
  {
    PyGILState_Release(gil);
    throw std::runtime_error("Python Engine: calc is not initialized");
  }

  double time;
  PyObject* args = PyTuple_New(2 + (int)additional_src.size());

  if (main_src.is_string)
  {
    time = main_src.str->at(point_index).x;
    std::string val(main_src.str->getString(main_src.str->at(point_index).y));
    PyTuple_SetItem(args, 0, PyFloat_FromDouble(time));
    PyTuple_SetItem(args, 1, PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size()));
  }
  else
  {
    const auto& p = main_src.numeric->at(point_index);
    time = p.x;
    PyTuple_SetItem(args, 0, PyFloat_FromDouble(time));
    PyTuple_SetItem(args, 1, PyFloat_FromDouble(p.y));
  }

  for (int i = 0; i < (int)additional_src.size(); i++)
  {
    const auto& src = additional_src[i];
    if (src.is_string)
    {
      int idx = src.str->getIndexFromX(time);
      std::string val =
          (idx != -1) ? std::string(src.str->getString(src.str->at(idx).y)) : std::string();
      PyTuple_SetItem(args, 2 + i, PyUnicode_FromStringAndSize(val.data(), (Py_ssize_t)val.size()));
    }
    else
    {
      int idx = src.numeric->getIndexFromX(time);
      double val = (idx != -1) ? src.numeric->at(idx).y : std::numeric_limits<double>::quiet_NaN();
      PyTuple_SetItem(args, 2 + i, PyFloat_FromDouble(val));
    }
  }

  PyObject* result = PyObject_CallObject(_py_calc, args);
  Py_DECREF(args);

  parsePythonResult(result, time, points, gil);
}

// Rebuild the Python engine after restoring the serialized state.
bool PythonCustomFunction::xmlLoadState(const QDomElement& parent_element)
{
  bool ret = CustomFunction::xmlLoadState(parent_element);
  initEngine();
  return ret;
}
