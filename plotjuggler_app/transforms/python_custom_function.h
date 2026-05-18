#ifndef PYTHON_CUSTOM_FUNCTION_H
#define PYTHON_CUSTOM_FUNCTION_H

#include "custom_function.h"

#pragma push_macro("slots")
#undef slots
#pragma push_macro("signals")
#undef signals
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#pragma pop_macro("signals")
#pragma pop_macro("slots")

#include <mutex>
#include <vector>
#include <string>

class PythonCustomFunction : public CustomFunction
{
public:
  PythonCustomFunction(SnippetData snippet = {});
  ~PythonCustomFunction() override;

  void initEngine() override;

  void calculatePoints(const MixedSource& main_src, const std::vector<MixedSource>& additional_src,
                       size_t point_index, std::vector<PlotData::Point>& points) override;

  QString language() const override
  {
    return "PYTHON";
  }

  const char* name() const override
  {
    return "PythonCustomFunction";
  }

  bool xmlLoadState(const QDomElement& parent_element) override;

  // Best-effort probe at app startup. Returns true if the embedded Python
  // interpreter initialized successfully; false if it is unavailable (in which
  // case any subsequent attempt to construct PythonCustomFunction will throw
  // a clean std::runtime_error rather than crashing the process).
  static bool probeAvailable();

  // True if a previous initialization attempt failed and Python is disabled
  // for the remainder of the process. Cheap; does not initialize anything.
  static bool isAvailable();

private:
  // Initializes the embedded Python interpreter once per process.
  static void ensurePythonInitialized();

  // Formats Python exceptions into user-facing PlotJuggler error messages.
  std::string fetchPythonExceptionWithTraceback();
  std::string formatError(const std::string& tb_text) const;

  // Parses the Python call result and fills points. Always releases the GIL.
  void parsePythonResult(PyObject* result, double time, std::vector<PlotData::Point>& points,
                         PyGILState_STATE gil);

  // Python execution context shared by globals and locals.
  PyObject* _globals = nullptr;
  PyObject* _locals = nullptr;

  // Cached reference to the user-defined calc(...) function.
  PyObject* _py_calc = nullptr;

  std::mutex mutex_;

  int global_lines_ = 0;
  int function_lines_ = 0;
};

#endif
