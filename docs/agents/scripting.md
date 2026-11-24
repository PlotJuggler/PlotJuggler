# Scripting reference

The languages an agent writes code in for PlotJuggler: per-sample filters, on-demand object scripts and marker rules.
The MCP tool descriptions give each tool's arguments; this page gives the language each tool runs. The authoritative
contracts are [`pj_scripting/docs/FILTER_CLASS.md`](../../pj_scripting/docs/FILTER_CLASS.md),
[`pj_scripting/include/pj_scripting/object_script.h`](../../pj_scripting/include/pj_scripting/object_script.h),
[`python_object_script.h`](../../pj_scripting/include/pj_scripting/python_object_script.h) and
[`marker_engine.h`](../../pj_scripting/include/pj_scripting/marker_engine.h).

## Which engine runs what

| Kind | Runs | Languages | MCP route | UI route |
|---|---|---|---|---|
| Filter (`transform`) | once per arriving sample, keeps state between samples, output is a time series | Luau, Python (desktop only) | `add_filter`, `add_script_series`, `create_derived_series` | Plot right-click → Apply Filter...; Custom Topics → + (Transform Editor) |
| On-demand script (`on_demand`) | once per requested time, stateless, outputs objects, numbers or strings | Luau, Python | `create_derived_object`, `evaluate` (Luau only) | Custom Topics → + (Transform Editor) |
| Marker rule (`markers`) | once over the whole series, emits plot markers | Luau | `create_markers` | Marker Editor toolbox |

Every script runs in a sandbox with a memory cap and an instruction or wall-clock budget (`BudgetLimits`,
`pj_scripting/include/pj_scripting/sandbox.h`); past it the node fails and emits nothing more. A script cannot read
files, the network or the clock.

## Built-in filters

`add_filter` takes the `id`; `params` override the defaults, keyed by parameter name. The same filters appear in the
plot's Apply Filter... panel under their names.

| id | Name | Parameters (`name`: type, label) | Output |
|---|---|---|---|
| `none` | None | none | input unchanged |
| `absolute` | Absolute | none | \|v\| |
| `scale` | Scale | `value_scale`: number, Scale; `value_offset`: number, Offset; `time_offset_sec`: number, Time offset (s) | `k*v + b`, shifted in time |
| `derivative` | Derivative | `use_custom_dt`: boolean, Use fixed Δt; `custom_dt`: number, Fixed Δt (s) | dv/dt; first sample dropped |
| `integral` | Integral | `use_custom_dt`, `custom_dt` as above | running trapezoidal integral; first sample dropped |
| `moving_average` | Moving Average | `window`: integer (samples), Window; `compensate_time_offset`: boolean, Center timestamp | boxcar mean |
| `moving_rms` | Moving Root Mean Squared | `window`: integer (samples) | RMS over the window |
| `moving_variance` | Moving Variance / Stdev | `window`: integer (samples); `std_dev`: boolean, Standard deviation | variance, or stdev |
| `outlier_removal` | Outlier Removal | `outlier_factor`: number, Factor | drops one-sample spikes; one-sample delay |
| `samples_counter` | Samples Counter | `samples_ms`: number (ms), Window | samples in the trailing window (int64) |
| `binary_filter` | Binary Filter | `binary_op`: enum 0 `== a`, 1 `< a`, 2 `<= a`, 3 `> a`, 4 `>= a`, 5 `in [a, b]`; `binary_a`, `binary_b`, `binary_tol`: number | 0/1 (int64) |
| `time_since_previous` | Time Since Previous | none | seconds since the previous sample; first sample dropped |

Windows count samples, not seconds: on a 1 kHz IMU a `window` of 100 is 0.1 s. The filters that read the value need
numbers: text that is not a number fails arithmetic (Luau reads number-like text as a number), naming the series;
`samples_counter` and `time_since_previous` read only the timestamps and accept one. Source:
`resources/filters/builtin_filters.luau`.

## Filters: the Luau class

What `add_script_series` (Luau) and the Transform Editor's filter scripts run. A script returns one class table.

```lua
-- pj-script: luau
local Lowpass = {
  id = "lowpass",                 -- add_script_series overwrites it with the series' own id
  name = "First-order low-pass",
  parameters = {
    { name = "tau", type = "number", default = 0.1, label = "Time constant", min = 1e-6 },
  },
}
Lowpass.__index = Lowpass

function Lowpass.create(params)   -- params holds every declared default, overlaid with the caller's values
  return setmetatable({ tau = params.tau, has_prev = false, pt = 0.0, y = 0.0 }, Lowpass)
end

function Lowpass:calculate(t, v)  -- t: seconds since this node's first sample; v: the input value
  if not self.has_prev then
    self.has_prev, self.pt, self.y = true, t, v
    return v
  end
  local a = (t - self.pt) / (self.tau + (t - self.pt))
  self.pt, self.y = t, self.y + a * (v - self.y)
  return self.y
end

return Lowpass
```

- **Class fields:** `id` (required), `name`, `description`, `version`, `output` (`"double"` default, `"int64"`, or
  `"same"` to keep the input's column kind, double for text), `parameters`, `create` (required).
- **Parameters:** an array of `{name, type, default, label, tooltip, min, max, step, decimals, unit, values,
  visible_when}`. Types: `number`, `integer`, `boolean`, `enum` (`values = { {value=, label=}, ... }`), `string`,
  `text`. The Transform Editor and the Apply Filter... panel generate a form from it. `visible_when = { param = "x",
  equals = v }` hides a field unless another parameter has that value.
- **`calculate` returns:** `nil` to emit nothing for this sample; one number, emitted at the input time; or
  `t_out, value` to emit at another time (single-input filters only).
- **State** lives in the instance (`self`). The host never calls `reset`; a recompute builds a fresh instance with
  `create`.
- **Several inputs:** `calculate(self, t, v, v1, v2, ...)`, the inputs already joined on equal timestamps (inputs that
  share no timestamp produce nothing, and `add_script_series` refuses them). Fields of one message always share
  timestamps; fields of different messages usually do not.
- **Several outputs:** return M numbers (`return a, b, c`); they map by position to the declared outputs
  (`add_script_series` `outputs`, which become `<name>/<output>`).
- **Errors are sticky:** a runtime error, a non-number result or an exhausted budget fails the node for good.
- **No `os`, `coroutine`, `debug`, `setfenv`, `getfenv`;** the standard library is read-only. A text input arrives
  as a string (`if v == "walking" then ...`); arithmetic on it fails the node ("Text inputs" below).
- **`t` is relative:** seconds since the first sample this node processed, not epoch time. Two filters on the same
  series may start at different instants.

A plain table with `calculate`/`reset` function fields and no metatable also works; its functions are called without
`self`. `add_script_series` accepts that shorter form:

```lua
return { name = "kmh", create = function(params)
  return { calculate = function(t, v) return v * 3.6 end }
end }
```

## Filters: `create_derived_series`

The host wraps `global` and `body` into a filter class, so an agent writes only statements:

- `global` runs once per instance; its locals persist across samples.
- `body` runs per sample with `time` (seconds since the node's first sample), `value` (first input) and `v1`, `v2`,
  ... (further inputs, joined on equal timestamps). It ends in `return`; returning nothing drops the sample; returning
  M values feeds M `outputs`.
- `expression` is shorthand for a `body` of `return <expression>`.

```text
global: local prev_t, prev_v = nil, nil
body:   if prev_t == nil then prev_t, prev_v = time, value; return end
        local d = (value - prev_v) / (time - prev_t)
        prev_t, prev_v = time, value
        return d
```

```text
inputs: ["/imu/orientation/x", "/imu/orientation/y", "/imu/orientation/z", "/imu/orientation/w"]
outputs: ["roll", "pitch", "yaw"]
body:   local x, y, z, w = value, v1, v2, v3
        local roll  = math.atan2(2*(w*x + y*z), 1 - 2*(x*x + y*y))
        local pitch = math.asin(math.clamp(2*(w*y - z*x), -1, 1))
        local yaw   = math.atan2(2*(w*z + x*y), 1 - 2*(y*y + z*z))
        return roll, pitch, yaw
```

### Text inputs

A text series (a ROS `std_msgs/String`, a CSV word column, another formula's `output_type` "string")
arrives as a string, so a formula compares it with a label. Counting the entries into `"falling"`:

```text
inputs: ["/robot_state/data"]
global: local prev = nil
        local falls = 0
body:   if value == "falling" and prev ~= "falling" then falls = falls + 1 end
        prev = value
        return falls
```

In Python `value` is a `str`: `return 1 if value == "falling" else 0`. Compare text, do not compute with it. In Luau,
text that is not a number fails arithmetic and the series with the engine's error (`attempt to perform arithmetic
(mul) on string and number`), while number-like text (`"1"`) reads as a number; in Python a `str` keeps its own
semantics (`value * 2` repeats it, `value + 1` raises).

## Filters: Python

Desktop only (the browser build has no Python filters). First line `# pj-script: python`, one top-level class `T`:

```python
# pj-script: python
import math

class T:
    id = "speed"                      # add_script_series overwrites it
    @staticmethod
    def create(params):               # params: a dict from the params JSON ({} when empty)
        return T()
    def calculate(self, time, value, *more):
        vx, vy = value, more[0]
        return math.hypot(vx, vy)     # a number, (out_time, value), a tuple of M numbers, or None
```

- Same time, return, join and sticky-error rules as Luau. Optional `name`, `description`, `version`, `output`,
  `reset`.
- No parameter schema: a Python filter gets no generated form; `create` reads the dict.
- Imports: `math`, `cmath`, `statistics` only. No `open`, `os`, `subprocess`.
- Each `calculate` runs on the GUI thread under a wall-clock watchdog: keep it cheap.

## On-demand object scripts

What `create_derived_object` and `evaluate` (object form) run, and the Transform Editor's object scripts. One run
computes one bundle at one requested time; nothing carries over between runs.

**Luau shape.** The tools wrap the `body`: `inputs` (a table keyed by each input's path, exactly as passed) and
`params` (the decoded `params` object) are already in scope. The body returns a table keyed by the declared output
names.

```lua
-- inputs: ["/lidar/points"]   outputs: ["near:kPointCloud", "closest_m:number", "marker:kSceneEntities"]
local cloud = inputs["/lidar/points"]
local near = cloud:crop_box{min = {-2, -2, -1}, max = {2, 2, 2}}
local top = near:extreme_point("z", "max")
if top == nil then
  return { near = near, closest_m = pj.unavailable("no points in the box"),
           marker = pj.scene.new{frame_id = cloud.frame_id, id = "top"}:finish() }
end
local marker = pj.scene.new{frame_id = cloud.frame_id, id = "top"}
  :sphere{center = {top.x, top.y, top.z}, diameter = 0.2, color = {255, 0, 0}}
  :text{position = {top.x, top.y, top.z + 0.3}, text = string.format("%.2f", top.z), font_size = 0.2}
return { near = near, closest_m = top.z, marker = marker:finish() }
```

**Python shape** (Transform Editor; the MCP object tools take Luau): a top-level `def evaluate(inputs, params)`
returning a dict.

```python
def evaluate(inputs, params):
    cloud = inputs["/lidar/points"]
    near = cloud.crop_box(min=(-2, -2, -1), max=(2, 2, 2))
    top = near.extreme_point("z", "max")
    if top is None:
        return {"near": near, "closest_m": pj.unavailable("no points in the box")}
    return {"near": near, "closest_m": top["z"]}
```

### Inputs, outputs, time

- **Outputs** are declared `"name:type"`: `number`, `string`, or a producible object type: `kPointCloud`,
  `kSceneEntities`, `kImageAnnotations`, `kImage`, `kDepthImage`, `kCameraInfo`, `kVoxelGrid`. An occupancy grid, a
  video frame and frame transforms cannot be outputs.
- **Binding the return value:** a keyed table binds by name (an undeclared key or a missing output is an error), an
  array by position, a single value to a single output. Every value must have its declared type; `nil` counts as
  missing.
- **`pj.unavailable(reason)`** marks one output unavailable for this run while the others carry values. It is not
  zero and not an error; a downstream node that needs it does not run.
- **Each input is resolved on its own,** at its newest sample at or before the requested time; no input is the clock
  of the others. The result is stamped with the newest resolved input time.
- **All inputs come from one dataset.** Inputs are declared up front; a script cannot discover topics.
- **Object fields:** an object is a read-only view of its SDK struct; `describe_topic` lists its fields and the
  operations below. Lists are read element by element. An int64 field (`timestamp_ns`) is a `pj.int64`: compare it
  with `==`, `<`, `<=`, take `a:diff_s(b)` (seconds) or `a:to_number()`.
- **A number output of a node with object inputs is a plottable series**, computed over the input's timestamps;
  its curve's legend reads "(computing N%)" until done.
- `math.random` is seeded the same on every run; there is no `os` and no clock.

### Point clouds

| Call | Returns |
|---|---|
| `cloud:count()` | number of points |
| `cloud:bounds()` | `{min={x,y,z}, max={x,y,z}, count=n}` over finite points, or `nil` |
| `cloud:extreme_point(channel, "max"\|"min")` | `{index (1-based), x, y, z, value}`, or `nil` |
| `cloud:crop_box{min={x,y,z}, max={x,y,z}}` | a new cloud inside the box |
| `cloud:filter(channel, op, value)` | a new cloud where `channel op value`; `op` one of `<`, `<=`, `>`, `>=`, `==`, `~=` |
| `cloud:transform(tf)` | the cloud moved by a `pj.tf.lookup` result, in its `target_frame` |
| `cloud:downsample(voxel_size)` | one centroid per occupied cube, an xyz cloud |
| `for i, x, y, z in cloud:xyz_points() do ... end` | the finite points, `i` 1-based |

Crop, filter and transform copy whole records, so intensity, RGB, ring and other channels survive.

### Images, depth, video

- `img:representation()`: `"jpeg"`, `"png"`, `"raw:<encoding>"`, `"compressed:<encoding>"` or `"depth:<encoding>"`.
- `img:decode()` → decoded pixels, or `nil, reason`. Original samples, no display scaling (a 16-bit depth keeps its
  millimetres). Decoded pixels have `width`, `height`, `channels`, `pixel(x, y)` (0-based; a number or
  `{r, g, b[, a]}`), `to_gray()`, `crop(x, y, w, h)` (keeps the source coordinates), `stats()` (`{min, max, mean}`) and
  `threshold(op, value[, channel])` → an `ImageAnnotations` of the matching pixels, or `pj.unavailable`.
- `depth:to_point_cloud(camera_info[, {step = n, max_depth = m}])` → an organized metric cloud in the camera frame
  (16UC1/mono16 in millimetres, 32FC1 in metres), or `pj.unavailable`. `camera_info` is a `CameraInfo` input.
- `video:frame_at(t)` → `{image, presentation_ns}`, or `nil, reason`; `t` an integer of nanoseconds or a `pj.int64`
  on the dataset clock.

### TF

- `pj.tf.now`: the requested instant, a `pj.int64`. Reading it makes the result depend on the cursor time.
- `pj.tf.lookup(target, source[, when])` → `{translation={x,y,z}, rotation={x,y,z,w}, target_frame}`, or
  `nil, reason`. No `when`: at `pj.tf.now`. A time: at that time. An object: at that object's own stamp (where its
  frame was when it was measured). Lookups hold the newest transform at or before the time, without interpolation.
- `pj.tf.frames()`: every frame name.

```lua
local cloud = inputs["/velodyne_points"]
local tf, why = pj.tf.lookup("map", cloud.frame_id, cloud)
if tf == nil then return { in_map = pj.unavailable(why) } end
return { in_map = cloud:transform(tf) }
```

### Builders

Builders chain and end with `:finish()`, which returns the object. Colours are `{r, g, b[, a]}` in 0-255; vectors are
`{x, y, z}` (Luau) or tuples (Python).

- `pj.scene.new{frame_id, id}` with `:sphere{center, diameter, color}`, `:cube{center, size, color}`,
  `:text{position, text, font_size, color, billboard}`, `:line_strip{points, thickness, color}`: a `SceneEntities`
  snapshot. Each evaluation replaces the whole set, so an empty builder clears it.
- `pj.annotations.new{image_topic}` with `:points{points, type, thickness, outline_color, fill_color}` (`type`:
  `points`, `line_list`, `line_strip`, `line_loop`), `:circle{center, diameter, thickness, outline_color,
  fill_color}`, `:text{position, text, font_size, text_color}`: pixel-space overlays for the image on
  `image_topic` (required). Naming an image input pairs them with that image and its stamp.
- `pj.cloud.new{preset, frame_id}` (`preset` `"xyz"`, `"xyzi"`, `"xyzrgb"`) with `:add(x, y, z[, intensity | r, g,
  b])` and `:add_points{{...}, ...}`.
- `pj.voxels.new{frame_id, origin, cell_size, dims}` with `:set(i, j, k, value)` (1-based): a dense `VoxelGrid` of one
  float32 `value` channel.

### Python idiom

Same operations, Python style: fields are attributes, indices 0-based, vectors tuples, `!=` for not-equal, builder
options as keyword arguments (`pj.scene.new(frame_id=..., id=...).sphere(center=(x, y, z), diameter=0.2)`), and a
fallible call returns `(value, None)` or `(None, reason)` where Luau returns `value` or `nil, reason`
(`pj.tf.lookup`, `decode()`, `frame_at`). `pj.tf.now` is an int. Imports: `math`, `cmath`, `statistics`, `json`, each
read-only; no `open`, `exec`, `eval`, `compile`, `getattr`, `setattr`, `type`.

## Marker rules

What `create_markers` (raw form) and the Marker Editor run: Luau, once over whole series. Times are nanoseconds on
the series' own timestamps.

```lua
-- shade while too fast
when("/robot/speed", above(2.0)):backgroundColor("red", { label = "too fast" })
-- a point where an overload starts, at most one per second
when("/imu/acc_x", outside(-3, 3)):event({ label = "overload", minGap = 1.0, severity = "warning" })
-- a line on every mode change, labelled old -> new
when("/robot/mode", changes()):verticalLine({ label = function(v, prev) return prev .. " -> " .. v end })
-- colour map: shade by value
when("/robot/mode"):backgroundColor({ [0] = "gray", [1] = "green", [2] = "orange", [3] = "red" })
```

**`when(series, condition?)`**: `series` is a name or a `series(...)` accessor; naming a series declares it as an
input. Conditions: `above(x)`, `below(x)`, `between(a, b)`, `outside(a, b)`, `equals(x)`, `isTrue()`, `changes()`,
or `function(v, t, prev, prev_t) return <boolean> end` (`prev` is `nil` at the first sample). Actions:

- `:backgroundColor(color?, opts?)`: a region per stretch where the condition holds (no colour: the severity colour).
  `opts.minDuration` (s) drops shorter regions.
- `:backgroundColor(map)` with no condition: colour by value. `map` is `{[value] = color}` or `function(v, t)`
  returning a colour, `{color = ..., label = ...}` or `nil`; a region spans each run of equal results.
- `:event(opts?)` (a point) / `:verticalLine(opts?)` (a line) where the condition starts to hold; `opts.every = true`
  marks every matching sample; `opts.minGap` (s) skips marks closer than that; `opts.label` may be
  `function(v, prev)`.

**Loops,** for windows and multi-step state:

| Function | Effect |
|---|---|
| `series("topic/field")` | accessor, `nil` if absent: `s:size()`, `s:at(i)` (0-based `{t, v}` or `nil`), `s:atTime(t)` (linear interpolation) |
| `startMarker(t)` / `closeMarker(t, opts?)` | open and close a time region |
| `createVerticalMarker(t, opts?)` | vertical line |
| `createHorizontalMarker(y, opts?)` | horizontal line |
| `createPointMarker(t, y, opts?)` | point |
| `createBandMarker(low, high, opts?)` | shaded value band |
| `createMarker(x?, y?, opts?)` | x only: vertical; y only: horizontal; both: point |
| `bandPower(s, f_lo, f_hi)` | summed FFT power in [f_lo, f_hi] Hz, DC removed |
| `GetSeriesNames()` | every available series name |

**`opts`:** `label` (pill text), `severity` (`"info"`, `"warning"`, `"error"`, `"critical"`; sets the default
colour), `color` (`"#rrggbb"`, `"#rgb"` or a Qt colour name such as `"orange"`; an unknown colour fails the rule),
`status` (`"none"`, `"pass"`, `"fail"`), `category`, `description`.

**Where markers show.** `create_markers` draws on the plots of its `output` series (default: the first input), or on
every plot with `"__global__"`. The Marker Editor's scope picks Timeseries (under the source series), Dataset (every
plot of the dataset) or Global (every loaded dataset). Marks fire once per stretch: a rule that yields hundreds of
markers usually needs a `minDuration`, a `minGap` or a slower signal.

```lua
-- "A then B within 2 s": a loop with state
local cmd, fb = series("/cmd/enable"), series("/status/enabled")
for i = 1, cmd:size() - 1 do
  local a, b = cmd:at(i - 1), cmd:at(i)
  if a.v == 0 and b.v == 1 and fb:atTime(b.t + 2e9) < 0.5 then
    createVerticalMarker(b.t, { label = "no enable feedback", severity = "error" })
  end
end
```
