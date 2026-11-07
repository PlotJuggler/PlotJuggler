<!-- SPDX-License-Identifier: MPL-2.0 -->
# PJ4 Filter Class Contract (Luau) + Parameter Schema

A PJ4 **data filter** is a self-describing **Luau class**. One file declares the
filter's identity, its **parameter schema** (from which the host *generates* the
editor panel — no hand-written Qt), and its behavior. The host reads the schema
*without instantiating* the filter (for the catalogue + the panel), and builds a
live instance only when the filter is applied.

This document is the source of truth for:
- the **filter-class module shape** (what a `.luau` file returns),
- the **parameter schema** that drives the generated `ParameterForm`,
- the **instance contract** (`calculate` / `reset` / `calculate_batch`),
- the **time/value contract**, and
- the **intentional differences from PJ3's `lua_custom_function`**.

---

## 1. Module shape

A filter file `return`s **one class table**, or a **list of class tables** (the
bundled builtins ship as one multi-class resource). Line 1 is the engine directive
`-- pj-script: luau`. The host reads it from the raw bytes to pick the backend of a
plugin-submitted transform (`DataProcessorService`; `pj-script: python` selects the
Python engine, §8); the engine itself ignores it, so the bundled resource's
`-- pj-filter: luau` is a plain comment.

```lua
-- pj-script: luau
-- SPDX-License-Identifier: MPL-2.0
return { <classA>, <classB>, ... }   -- or a single class table
```

A class table:

| field | required | meaning |
|---|---|---|
| `id` | yes | stable catalogue key; must be byte-identical to the builtin id so old layouts resolve |
| `name` | yes | human type name shown in the filter list |
| `description` | no | catalogue / tooltip |
| `version` | no | semver, for the marketplace (deferred) |
| `output` | no (⇒ `"double"`) | `"double"` \| `"int64"` \| `"same"` — the stored `StorageKind` |
| `parameters` | no (⇒ none) | the schema array below |
| `create` | yes | `function(params) -> instance` factory |

The host reads `id/name/description/version/output/parameters` **without** calling
`create` (so a class whose `create` errors still appears in the catalogue).

---

## 2. Parameter schema → generated `ParameterForm`

`parameters` is an **array** of descriptor tables. Each generates one widget; the
order is the panel order. `name` is also the **JSON key** used by the filter's
params (so `name` must match what `create(params)` reads).

| field | applies to | meaning |
|---|---|---|
| `name` | all (required) | params-table key + JSON key |
| `type` | all (required) | one of the types below |
| `default` | all (required) | typed default value |
| `label` | all | UI label (falls back to `name`) |
| `tooltip` | all | hover help |
| `min`, `max`, `step` | number/integer | spin clamp + step |
| `decimals` | number | displayed precision |
| `unit` | number/integer | suffix shown after the value (e.g. `"s"`) |
| `values` | enum | options: `{ {value=, label=}, ... }` (or bare strings) |
| `visible_when` | all | `{ param="other", equals=<scalar> }` — show only when conditioned |

### Type → widget

| `type` | widget | stored value |
|---|---|---|
| `"number"` | `QDoubleSpinBox` / validated line edit | double |
| `"integer"` | `QSpinBox` | int |
| `"boolean"` | `PJ::CheckButton` / `ToggleSwitch` | bool |
| `"enum"` | `PJ::ComboBox` | the option's **string** `value` |
| `"string"` | `QLineEdit` | string |
| `"text"` | `QPlainTextEdit` | string (multi-line, e.g. an expression body) |

The form binds generically through the `saveParams()`/`loadParams()` JSON seam:
the param `name` IS the JSON key, so no per-filter `static_cast` is needed.

`create(params)` always receives a **complete** params table: the engine seeds
every declared default first, then overlays the caller's values (so `"{}"` still
yields all defaults).

---

## 3. Instance contract

`create(params)` returns a **live instance** exposing `calculate` (required) and
`reset` (optional). Author it as an idiomatic Luau **class** — state in `self`,
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
walk — a `__index` *function* is never invoked during binding) and calls them with
the instance bound as `self`. **State therefore lives in the instance table, and the
host keeps that table alive for the filter's lifetime.**

> A plain instance table with direct `calculate`/`reset` *function* fields (no
> metatable) is still accepted and called **without** `self` — the host detects which
> form an instance uses. The class form above is the recommended idiom.

- **`calculate(self, t, v)`** — once per sample. Returns:
  - `nil` / no value → **suppress** (emit nothing; e.g. a derivative's first sample),
  - one number → emit that value at the input time,
  - two numbers → `(t_out, value)`, emit at an explicit time.
- **`reset(self)`** — optional; clears state. The host usually prefers re-creating
  the instance (construct-new-and-swap) — see §5.

A runtime error raised in `calculate` **fails the node** (sticky) rather than
silently corrupting a stateful accumulator.

---

## 4. Time / value contract

- `t` = **seconds since session start** (a double), and `v` = the sample value (a
  double).
- The host rebases time on the **int64 nanosecond spine** (`(raw_ns − session_t0_ns)`)
  **before** the double cast, and reconstructs the absolute spine on output. This
  is mandatory: an absolute epoch timestamp in a `double` quantizes to ~256 ns
  (53-bit mantissa at magnitude ~2⁶⁰) and would corrupt every `dt`-based filter via
  catastrophic cancellation. Session-relative time is sub-picosecond.
- Consequence for authors: `t` differences (`dt`) are exact; do **not** assume `t`
  is wall-clock epoch.

---

## 5. Differences from PJ3 `lua_custom_function` (intentional)

These diverge from PJ3 by design — PJ4 is greenfield, not 3.x-layout-compatible:

| aspect | PJ3 | PJ4 | why |
|---|---|---|---|
| time arg | raw absolute `p.x` | seconds since session start (rebased int64) | precision (see §4) |
| `reset()` | no-op (a known streaming-crash workaround) | construct-new-and-swap (clears state) | the eager engine recomputes from start; state **must** clear or the integral double-counts |
| error | throws, aborts the commit | sticky-fail the node, suppresses | one bad sample shouldn't abort ingest |
| return shapes | value / (t,v) / **table of rows** | value / (t,v) / nil; a MIMO class returns M numbers (§7) | one row per input sample; there is no table-of-rows form |
| inputs | main + extra sources (`v1, v2, …`) | one input (SISO), or N inputs joined to one timestamp (MIMO, §7) | the engine node owns the join, not the script |
| globals | standard Lua libs, no injected helpers | standard Luau libs, no injected helpers | parity |

Parity (no change): the script owns its own previous-value/first-point state via
closure upvalues; no `v_prev` global is injected.

---

## 5a. Double-only-engine limitations (documented inexactness)

Luau's only number type is `double`, and the host hands filters `double` time/value.
A few builtins therefore have narrow, **documented** inexactness (their behavior,
including these within-range limits, is golden-tested by `builtin_golden_test`):

- **Mirror builtins** (`none`, `absolute`, `outlier_removal`) preserve an int64/uint64
  column kind, but a value above **2^53** loses its low bits through the double seam.
  Within ±2^53 they are exact (tested); strings are never fed to a filter (series are
  numeric), so the string-mirror case does not arise in practice.
- **`time_since_previous`** emits a ns delta reconstructed as `(t − prev_t)·1e9`; exact
  while session-relative ns stay below 2^53 (~104 days), then 1-ns deltas can quantize.
- **Explicit-timestamp filters** (`scale` time-offset, `moving_average` compensate
  midpoint) reconstruct the int64 spine through double seconds, so an odd-span midpoint
  can differ from C++'s integer-ns division by **±1 ns**.
- **`samples_counter`** compares a double-seconds window bound; a sample sitting exactly
  on `current − window` can flip membership once that ns boundary is unrepresentable.

These are inherent to choosing a double-only engine (the substrate trade-off) and are
acceptable for v1; a future C++ accelerator (or a Luau `buffer`/int path) could remove
them if a use case demands bit-exactness beyond 2^53.

## 6. Full example — derivative

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

## 7. MIMO (N inputs → M outputs)

The SISO contract above (§3, one input value → one output value) is the 1→1 case.
A class may instead be **MIMO**: it reads **N** inputs and emits **M** outputs.

- **`calculate`** receives the primary value plus the secondary inputs as extra
  positional args: `calculate(self, t, v, v1, v2, …, vN-1)`. `t` is still seconds
  since session start; `v` is input 0 (the primary), `v1..vN-1` the secondary
  inputs. The inputs arrive **already aligned to one timestamp** — the *join* (how
  the secondary inputs are matched to the primary's sample) is the engine node's
  concern, **not** the script's. (Exact-timestamp join and nearest-sample-to-primary
  are two different engine nodes; the script is agnostic to which one drives it.)
- **Returns M numbers** (a Luau multi-return): `return out0, out1, …, outM-1`. They
  map **positionally** to the M output topics declared at install time. Returning
  `nil` (or nothing) as the first result **suppresses** the row (nothing emitted).
- The SISO two-number `(t_out, value)` explicit-timestamp form is **SISO-only**. In
  MIMO every return is an output value; the M outputs share the joined input timestamp.
- The class's single `output` kind applies to **all** M outputs (`"mirror"`/`"same"`
  mirrors the primary input's kind).

Host side: a MIMO class is installed as a `LuaMimoTransform` (a `PJ::IMIMOTransform`),
the multi-input sibling of `LuaSisoTransform`. Time contract, sandbox/watchdog and
sticky-failure behaviour are identical to SISO.

---

## 8. Python filters

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
    output = "double"       # optional: "double" (default) | "int64" | "same"

    @staticmethod
    def create(params):     # params: a dict decoded from the params JSON ({} when empty)
        return T()

    def reset(self):        # optional
        ...

    def calculate(self, time, value, *args):
        return value * 2    # a number | (out_time, value) | None to suppress
```

- **Same contracts as Luau.** The instance contract (§3) and the time/value contract (§4)
  hold unchanged: `time` is seconds since session start, a number is emitted at the input
  time, a two-element `(out_time, value)` at an explicit time, `None` suppresses, a runtime
  error fails the node (sticky).
- **MIMO.** The secondary inputs arrive in `*args`, and `calculate` returns a tuple or list
  of M numbers (a bare number is one output; `None` suppresses).
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
  a sandbox (`python_engine.h`; `docs/derived_objects/ARCHITECTURE.md` §14).
- **Where it runs.** On the GUI thread, like every filter (`IncrementalEngine`), holding the
  GIL for each call. It is unavailable in the browser build.

---

## Implementation pointers
- Schema/types: `pj_scripting/include/pj_scripting/filter_class.h` (`ParamSpec`, `FilterClass`).
- Engine seam: `pj_scripting/include/pj_scripting/script_engine.h` (`ScriptEngine`, `FilterInstance`, `makeLuauEngine`); `pj_scripting/include/pj_scripting/python_engine.h` (`makePythonEngine`, the Python module contract).
- Luau binding: `pj_scripting/src/luau_engine.cpp` (hand-written — NOT sol2; Luau headers are C++-linkage, `lua_ref` does not pop).
- Engine node: `pj_scripting/include/pj_scripting/lua_siso_transform.h` (`LuaSisoTransform : PJ::proc::DataProcessor`, the data-processor base in `pj_datastore`; the existing `ProcessorSisoAdapter` installs it as a `IncrementalEngine` node).
- The generated panel (`ParameterForm`) is built in `pj_plotting/widget`.
