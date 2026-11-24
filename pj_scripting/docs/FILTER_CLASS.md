<!-- SPDX-License-Identifier: MPL-2.0 -->
# PJ4 Filter Class Contract (Luau) + Parameter Schema

A PJ4 **data filter** is a self-describing **Luau class**. One file declares the
filter's identity, its **parameter schema** (from which the host *generates* the
editor panel, no hand-written Qt), and its behavior. The host reads the schema
*without instantiating* the filter (for the catalogue and the panel), and builds a
live instance only when the filter is applied.

This document is the reference for:
- the **filter-class module shape** (what a `.luau` file returns),
- the **parameter schema** that drives the generated `ParameterForm`,
- the **instance contract** (`calculate` / `reset`),
- the **time/value contract**, and
- **MIMO** classes and **Python** filters.

---

## Module shape

A filter file `return`s **one class table**, or a **list of class tables** (the
bundled builtins ship as one multi-class resource). Line 1 is the engine directive
`-- pj-script: luau`. The host reads it from the raw bytes to pick the backend of a
plugin-submitted transform (`DataProcessorService`; a script without a recognized
directive is refused, and `pj-script: python` selects the Python engine, see
"Python filters"); the engine itself ignores it, so the bundled resource's
`-- pj-filter: luau` is a plain comment.

```lua
-- pj-script: luau
-- SPDX-License-Identifier: MPL-2.0
return { <classA>, <classB>, ... }   -- or a single class table
```

A class table:

| field | required | meaning |
|---|---|---|
| `id` | yes | stable catalogue key; a builtin's id must not change, so old layouts resolve |
| `name` | no (⇒ `id`) | human type name shown in the filter list |
| `description` | no | catalogue / tooltip |
| `version` | no | semver string |
| `output` | no (⇒ `"double"`) | `"double"` \| `"int64"` \| `"same"` (`"mirror"` is an alias) \| `"string"`: the stored column kind; `"same"` keeps the input column's kind (a text input gives `"double"`); `"string"` stores text ("Text outputs") |
| `parameters` | no (⇒ none) | the schema array below |
| `create` | yes | `function(params) -> instance` factory |

The host reads `id/name/description/version/output/parameters` **without** calling
`create` (so a class whose `create` errors still appears in the catalogue).

---

## Parameter schema → generated `ParameterForm`

`parameters` is an **array** of descriptor tables. Each generates one widget; the
order is the panel order. `name` is also the **JSON key** used by the filter's
params (so `name` must match what `create(params)` reads).

| field | applies to | meaning |
|---|---|---|
| `name` | all (required) | params-table key + JSON key |
| `type` | all (⇒ `"number"`) | one of the types below; an unknown type is a class error |
| `default` | all | typed default value; without one the key is absent from `params` |
| `label` | all | UI label (falls back to `name`) |
| `tooltip` | all | hover help on the label |
| `min`, `max` | number/integer | bounds: a number's validator, an integer's spin range |
| `step` | integer | spin step (ignored for a number) |
| `decimals` | number | allowed decimals (0 to 17) |
| `unit` | number/integer | parsed into the schema; the generated form does not display it |
| `values` | enum | options: `{ {value=, label=}, ... }` or bare scalars (a bare scalar is both value and label) |
| `visible_when` | all | `{ param="other", equals=<scalar> }`: show only when the other parameter equals the value |

### Type → widget

| `type` | widget | stored value |
|---|---|---|
| `"number"` (aliases `"double"`, `"float"`) | validated line edit | double |
| `"integer"` (`"int"`) | `QSpinBox` (32-bit: `min` / `max` clamp to the int range, default range ±1e9) | int |
| `"boolean"` (`"bool"`) | `PJ::ToggleSwitch` | bool |
| `"enum"` (`"choice"`) | `PJ::ComboBox` | the chosen option's `value`, a JSON scalar (an int-keyed enum stays a number) |
| `"string"` | `QLineEdit` | string |
| `"text"` (`"multiline"`) | `QPlainTextEdit` | string (multi-line, e.g. an expression body) |

The form binds generically through the `saveParams()`/`loadParams()` JSON seam:
the param `name` IS the JSON key, so no per-filter `static_cast` is needed.

`create(params)` always receives a table seeded with **every declared default**,
overlaid with the caller's values (so `"{}"` still yields all defaults; malformed
params JSON leaves the defaults). Values are scalars: numbers, booleans, strings.

---

## Instance contract

`create(params)` returns a **live instance** exposing `calculate` (required) and
`reset` (optional). Author it as an idiomatic Luau **class**: state in `self`,
methods shared on the class via `__index`:

```lua
local Filter = { id = "my_filter", name = "My Filter", parameters = { ... } }
Filter.__index = Filter

function Filter.create(params)                  -- static factory → instance
  return setmetatable({ --[[ per-instance state ]] }, Filter)
end

function Filter:reset() ... end                  -- optional; `self` = the instance
function Filter:calculate(t, v) ... end          -- REQUIRED; `self`, then (t, v)

return Filter
```

The host resolves `calculate`/`reset` through the metatable's `__index` (a **raw**
walk: a `__index` *function* is never invoked during binding) and calls them with
the instance bound as `self`. **State therefore lives in the instance table, and the
host keeps that table alive for the filter's lifetime.**

> A plain instance table with direct `calculate`/`reset` *function* fields (no
> metatable) is still accepted and called **without** `self`: the host detects which
> form an instance uses. The class form above is the recommended idiom.

- **`calculate(self, t, v)`**, once per sample. Returns:
  - `nil` / no value → **suppress** (emit nothing; e.g. a derivative's first sample),
  - one number → emit that value at the input time,
  - two numbers → `(t_out, value)`, emit at an explicit time. Both must be numbers.
    A non-finite or out-of-range `t_out` suppresses the row.
- **`reset(self)`**: optional; clears state. The host does not call it: it rebuilds
  the instance from `create` (construct-new-and-swap), so a stateful filter starts
  clean on every recompute.

A runtime error raised in `calculate`, a result that is not a number, a
`(number, non-number)` pair, or an exceeded budget **fails the node** (sticky:
nothing is emitted from then on) rather than silently corrupting a stateful
accumulator.

### Text outputs

A class with `output = "string"` returns text where the others return numbers: a string, `(t_out, string)`,
or `nil`, and in MIMO one string per output. The output is a string series (a state label per sample, such as
`"walking"` or `"falling"`): the State Transitions panel and the curve list's value column show it, plots do not.
A number from a text class fails the node, and so does a string from a numeric class (the error names
`output = "string"`); in Luau a numeric string such as `"5"` from a numeric class is still read as a number
(Python needs a number). Editing a
transform in place cannot switch it between text and numbers (its output column keeps its kind): remove it and
create it again.

Each filter runs in its own sandboxed VM with a memory cap and an instruction
budget (`BudgetLimits`, `sandbox.h`). `os`, `coroutine`, `debug`, `setfenv` and
`getfenv` are not available, and the standard library is read-only; top-level
globals are the VM's own and persist for the instance.

---

## Time / value contract

- `t` = **seconds since the first sample the filter processes** (a double), and
  `v` = the sample value: a number (an integer input widens to a double), or a string for a text input. The origin
  is captured again after a reset.
  Do not assume `t` is wall-clock or epoch time, or that two filters on the same
  series started at the same instant.
- The host rebases time on the **int64 nanosecond spine** (`raw_ns − first_sample_ns`)
  **before** the double cast, and reconstructs the absolute spine on output. This
  is mandatory: an absolute epoch timestamp in a `double` quantizes to ~256 ns
  (53-bit mantissa at magnitude ~2⁶⁰) and would corrupt every `dt`-based filter via
  catastrophic cancellation. A first-sample-relative double steps by about 0.5 ps
  after an hour and stays within 1 ns up to about 97 days.
- Consequence for authors: `t` differences (`dt`) are accurate to that resolution.
- Text inputs: a text column (`kString`) reaches the filter as a Luau string or a Python `str`, so `v == "walking"`
  works. In Luau, text that is not a number fails arithmetic (Luau reads number-like text such as `"1"` as a number),
  and an ordered comparison of text with a number fails too: the runtime error fails the filter instead of reading the
  text as 0. A Python `str` keeps its own semantics (`v * 2` repeats it). The bundled filters that read the value need
  numbers; `samples_counter` and `time_since_previous` look only at the time and accept text.

---

## Differences from PJ3 `lua_custom_function`

PJ3 custom Lua functions are not source-compatible. In PJ4:

- the time argument is the rebased `t` above, not an absolute timestamp;
- `reset()` rebuilds the instance, so state always clears;
- a script error fails the node instead of aborting the commit;
- a filter returns a value, `(t, v)`, or `nil` per input sample: there is no
  table-of-rows form;
- a SISO filter has one input, and a MIMO class N inputs joined to one timestamp
  (see "MIMO"); the engine node owns the join, not the script;
- no helper globals are injected: the script keeps its own previous-value state.

---

## Double-only engine limitations

Luau's only number type is `double`, and the host hands filters a `double` time and numeric values as `double`.
A few builtins therefore have narrow, **documented** inexactness (their behavior,
including these within-range limits, is golden-tested by `builtin_golden_test`):

- **Mirror builtins** (`none`, `absolute`, `outlier_removal`) preserve an int64/uint64
  column kind, but a value above **2^53** loses its low bits through the double seam.
  Within ±2^53 they are exact (tested).
- **`time_since_previous`** emits the gap in seconds as a double `(t − prev_t)`; its
  resolution is the double's at the session-relative magnitude (see above).
- **Explicit-timestamp filters** (`scale` time offset, `moving_average` with a
  centred timestamp) return `t_out` in double seconds, which the host rounds onto the
  int64 spine, so an odd-span midpoint can be off by **1 ns**.
- **`samples_counter`** compares a double-seconds window bound; a sample sitting exactly
  on `current − window` can flip membership once that ns boundary is unrepresentable.

These follow from running filters on a double-only engine; a C++ accelerator or a
Luau `buffer`/int path would be needed for bit-exactness beyond 2^53.

## Full example: derivative

```lua
-- pj-script: luau
local Derivative = {
  id = "derivative", name = "Derivative", description = "dv/dt", version = "1.0.0",
  parameters = {
    { name="use_custom_dt", type="boolean", default=false, label="Use fixed Δt",
      tooltip="Divide by a constant step instead of the sample interval." },
    { name="custom_dt", type="number", default=0.01, label="Fixed Δt", unit="s",
      min=1e-9, step=0.001, decimals=6,
      visible_when={ param="use_custom_dt", equals=true } },
  },
}
Derivative.__index = Derivative

function Derivative.create(params)
  return setmetatable(
    { use_custom = params.use_custom_dt, fixed = params.custom_dt, has_prev = false, pt = 0.0, pv = 0.0 },
    Derivative)
end

function Derivative:reset()
  self.has_prev = false
end

function Derivative:calculate(t, v)
  if not self.has_prev then self.has_prev, self.pt, self.pv = true, t, v; return nil end
  local dt = self.use_custom and self.fixed or (t - self.pt)
  local d = (dt ~= 0.0) and (v - self.pv) / dt or 0.0
  self.pt, self.pv = t, v
  return d
end

return Derivative
```

`resources/filters/builtin_filters.luau` is the 12-class reference (the bundled list
form: `return { Derivative, Integral, ... }`).

---

## MIMO

The SISO contract above (one input value → one output value) is the 1→1 case.
A class may instead be **MIMO**: it reads **N** inputs and emits **M** outputs.

- **`calculate`** receives the primary value plus the secondary inputs as extra
  positional args: `calculate(self, t, v, v1, v2, …, vN-1)`. `t` is as above;
  `v` is input 0 (the primary), `v1..vN-1` the secondary inputs. The inputs arrive
  **already aligned to one timestamp**: the *join* (how the secondary inputs are
  matched to the primary's sample) is the engine node's concern, **not** the
  script's. (Exact-timestamp join and nearest-sample-to-primary are two different
  engine nodes; the script is agnostic to which one drives it.)
- **Returns M numbers** (M strings for a text class) as a Luau multi-return: `return out0, out1, …, outM-1`. They
  map **positionally** to the M output topics declared at install time. Returning
  `nil` (or nothing) as the first result **suppresses** the row (nothing emitted).
  A non-number fails the node; a result count other than M produces no row and
  records an error message.
- The SISO two-number `(t_out, value)` explicit-timestamp form is **SISO-only**. In
  MIMO every return is an output value; the M outputs share the joined input timestamp.
- The class's single `output` kind applies to **all** M outputs (`"same"` mirrors
  input 0's kind, float64 for text).

Host side: a MIMO class is installed as a `LuaMimoTransform` (a `PJ::IMIMOTransform`),
the multi-input sibling of `LuaSisoTransform`. Time contract, sandbox/watchdog and
sticky failure on a script error are identical to SISO.

---

## Python filters

A filter may be a Python module instead of a Luau one (`makePythonEngine`, embedded
CPython, in-process). Line 1 is `# pj-script: python`: the host reads it to route the
source to the Python engine. The module defines **one** top-level class named `T` (not a
list of classes):

```python
# pj-script: python
class T:
    id = "my_filter"        # required, non-empty
    name = "My Filter"      # optional, defaults to id
    description = ""        # optional
    version = ""            # optional
    output = "double"       # optional: "double" (default) | "int64" | "same" | "string"

    @staticmethod
    def create(params):     # params: a dict decoded from the params JSON ({} when empty)
        return T()

    def reset(self):        # optional
        ...

    def calculate(self, time, value, *args):
        return value * 2    # a number | (out_time, value) | None to suppress
```

- **Same contracts as Luau.** The instance contract and the time/value contract
  hold unchanged: `time` is seconds since the first sample, a number is emitted at the
  input time, a two-element `(out_time, value)` at an explicit time, `None` suppresses, a
  runtime error fails the node (sticky).
- **MIMO.** The secondary inputs arrive in `*args`, and `calculate` returns a tuple or list
  of M numbers (a bare number is one output; `None` suppresses).
- **Text.** A text input arrives as a `str`. With `output = "string"`, every returned value is a `str` instead
  ("Text outputs").
- **No parameter schema.** The engine reads only `id`, `name`, `description`, `version` and
  `output` from the class, so a Python filter gets no generated `ParameterForm`; `create`
  receives the params JSON as a dict.
- **Imports.** Only `math`, `cmath` and `statistics`; any other import is refused with an
  error naming the allowed set.
- **Limits** (`BudgetLimits`, `sandbox.h`): each `calculate()` / `reset()` runs under a
  wall-clock watchdog (`python_call_millis`), module evaluation and `create()` under
  `python_watchdog_millis`, and an instance's allocations, across its whole life, under
  `mem_bytes`. Past any of them the instance fails for good, even if the script caught the
  error. The audit hook refuses `open`, `os.*` and `subprocess.*`. This is a guardrail, not
  a sandbox (`python_engine.h`; `docs/derived_objects/ARCHITECTURE.md` "Scripting languages").
- **Where it runs.** On the GUI thread, like every filter (`IncrementalEngine`), holding the
  GIL for each call. It is unavailable in the browser build.

---

## Implementation pointers
- Schema/types: `pj_scripting/include/pj_scripting/filter_class.h` (`ParamSpec`, `FilterClass`).
- Engine seam: `pj_scripting/include/pj_scripting/script_engine.h` (`ScriptEngine`, `FilterInstance`, `makeLuauEngine`); `pj_scripting/include/pj_scripting/python_engine.h` (`makePythonEngine`, the Python module contract).
- Luau binding: `pj_scripting/src/luau_engine.cpp` (hand-written, not sol2; Luau headers are C++-linkage and `lua_ref` does not pop).
- Engine node: `pj_scripting/include/pj_scripting/lua_siso_transform.h` (`LuaSisoTransform : PJ::proc::DataProcessor`, the data-processor base in `pj_datastore`; the existing `ProcessorSisoAdapter` installs it as a `IncrementalEngine` node).
- The generated panel (`ParameterForm`) is built in `pj_plotting/widget`.
