// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/expected.hpp"
#include "pj_base/span.hpp"
#include "pj_base/types.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_scripting/sandbox.h"
#include "pj_tf/tf_buffer.h"

// The Luau object-script engine the on-demand service calls on a worker thread
// (docs/derived_objects/ARCHITECTURE.md §5-7, §12, §14): a stateless evaluation over named
// scalar and object inputs that returns one value per output. Compile once per recipe revision; each thread keeps
// one sandboxed state, and every run gets a fresh thread on it with its own globals, its own random seed and a full
// collection when it ends, so two runs never see each other's state (D2, GAP-38).

namespace PJ::scripting {

namespace native {
class MemoryBudget;
}  // namespace native
class IImageDecoder;
class IVideoFrameSource;

/// An output a script marked unavailable for this run (`pj.unavailable(reason)`, D25). Distinct
/// from zero, from an empty object and from a script error.
struct Unavailable {
  std::string reason;
};

/// One value a script reads or returns.
using ScriptValue = std::variant<double, std::string, sdk::BuiltinObject, Unavailable>;

enum class OutputType : uint8_t {
  kNumber,
  kString,
  kObject,
};

/// One declared output: its name and fixed type (D8).
struct OutputSpec {
  std::string name;
  OutputType type = OutputType::kNumber;
  sdk::BuiltinObjectType object_type = sdk::BuiltinObjectType::kNone;  ///< kObject only

  bool operator==(const OutputSpec&) const = default;
};

/// Parses one `"name:type"` declaration (e.g. `"max_z:number"`, `"cropped:kPointCloud"`); the name and
/// the type are trimmed. `type` is `number`, `string` or an object type a script can produce: point
/// cloud, scene entities, image annotations, image, depth image or camera info. Rejects an empty name, a
/// name holding whitespace or `:`, a missing or doubled `:`, an empty or unknown type, and an object type
/// no script can produce (an occupancy grid; a video frame or frame transforms until a slice decides
/// how they publish). Duplicate names across a list are the caller's check.
[[nodiscard]] Expected<OutputSpec> parseOutputSpec(std::string_view spec);
/// The type half of `spec`'s declaration, as `parseOutputSpec` reads it: `number`, `string` or the object
/// type's name (`kPointCloud`).
[[nodiscard]] std::string outputTypeName(const OutputSpec& spec);

/// A resolved input or a produced output, named as the script sees it.
struct NamedValue {
  std::string name;
  /// What `value` must not outlive, declared before it so it is released after it. An input's: what keeps the code
  /// that made it loaded (a parser's library); a Python run's proxies hold it, since a script can keep an object past
  /// its run. An output's: its native charge (a builder's output has no buffer anchor to carry one) and, for a returned
  /// input, that input's keepalive, so a retained result keeps counting against its ledger.
  std::shared_ptr<const void> keepalive = {};
  ScriptValue value;
  /// Inputs only: the store entry's `payload_stamp_shift`. A merge moves the dataset clock but leaves
  /// the stamps inside payloads where they were; the engine adds this to an object input's own stamp
  /// (its top-level `timestamp_ns`, or `timestamp`; saturating at the int64 limits) before the script sees
  /// it, as TF ingest does to TF header stamps. A script therefore reads every stamp on the dataset clock, and so do
  /// its outputs. A stamp of 0 means unstamped and stays 0, except on an Image or DepthImage, where it is a stamp
  /// (annotations pair with it) and moves too.
  Timestamp stamp_shift = 0;
  /// Object inputs only: the store topic the entry was read from (bare, no dataset qualifier), which the
  /// annotations made from an image input pair with (Scene2D matches `image_topic` to it).
  std::string topic = {};
  /// Object inputs only: that store topic's id, what `video:frame_at` decodes from; none for an object with no store
  /// topic (an evaluation's chained output).
  ObjectTopicId topic_id = {};
};

/// Approximate heap bytes of `value`, read from its decoded fields (a buffer's size, strings, list
/// elements) and never by serializing it (docs/derived_objects/ARCHITECTURE.md §9, requirement 1):
/// sizing a 3 MB scan costs a walk of its field table, not a copy of its points.
[[nodiscard]] std::size_t estimateBytes(const ScriptValue& value);

/// One run's result.
struct ScriptBundle {
  /// With declared outputs: one entry per declaration, in its order. Without: the outputs the run
  /// returned, named by the inference rule (see `runOnDemandScript`); an output's type is its value's.
  std::vector<NamedValue> outputs;
  /// The run read the query clock (`pj.tf.now`, or a `pj.tf.lookup` with no time): the requested
  /// instant is part of the result's identity (ARCHITECTURE §10).
  bool observed_query_clock = false;
  /// The run read the TF tree (`pj.tf.lookup`, `pj.tf.frames`, with or without a snapshot): the
  /// snapshot's revision is part of the result's identity.
  bool observed_tf = false;
};

/// Why a run failed, with what it had observed by then: a failure that read the TF tree (or the clock)
/// may succeed at another revision (or instant), one that did not will fail again on the same inputs.
struct ScriptError {
  std::string message;
  bool observed_query_clock = false;
  bool observed_tf = false;
  bool over_budget = false;  ///< a memory limit ended the run: the VM's cap or the native budget
};

/// Luau bytecode of one script; it carries no state, so compile it once per recipe revision.
struct CompiledScript {
  std::vector<char> bytecode;
};

/// Compiles `source`, a chunk run as `chunk(inputs, params)`. A syntax error is reported here, with
/// its line.
[[nodiscard]] Expected<CompiledScript> compileOnDemandScript(std::string_view source);

/// A native-allocation ledger of `limit_bytes` for runs to share (`RunOptions::native_budget`). Thread-safe.
[[nodiscard]] std::shared_ptr<native::MemoryBudget> makeNativeBudget(std::size_t limit_bytes);

/// The native budget's default: native outputs (crops, filters, transforms) and output strings of one
/// run, 64 MiB. A 128-beam lidar scan's transform needs about 13 MB.
inline constexpr std::size_t kDefaultNativeBudgetBytes = std::size_t{64} << 20;
/// The deepest `params` a run accepts: each level takes Lua stack slots while it is pushed.
inline constexpr int kMaxParamsDepth = 32;

/// What a run may read besides its inputs, and what it may spend.
struct RunOptions {
  /// The dataset's TF state; null when the dataset has no TF. It may hold the fixed-joint edges a 3D
  /// dock's robot model writes into the dataset's buffer: they only add frames and never change a lookup
  /// between frames the recording publishes.
  std::shared_ptr<const pj::tf::TransformSnapshot> tf;
  /// The query instant, on the dataset clock: what `pj.tf.now` reads and where a `pj.tf.lookup` with no
  /// time looks, in the entry clock (ARCHITECTURE §12).
  Timestamp query_time = 0;
  /// Polled inside the interpreter and by the native operations (empty: never cancelled); a cancelled
  /// run fails with "script cancelled" even when the script `pcall`s the error.
  std::function<bool()> cancelled;
  /// The same cancellation as thread-safe flags (any set: cancelled), which the Python watchdog polls from its own
  /// thread to interrupt a script inside its bytecode; Luau polls `cancelled` in its interrupt instead.
  std::vector<std::shared_ptr<const std::atomic<bool>>> cancel_flags;
  /// Bounds the native outputs the run holds at once and the strings it returns: a ledger separate from
  /// `limits.mem_bytes`, the VM's own cap. An output the script retains past the run keeps its charge
  /// until it dies.
  std::size_t native_budget_bytes = kDefaultNativeBudgetBytes;
  /// Set: the run charges this ledger (`makeNativeBudget`) instead of a fresh one of `native_budget_bytes`, so runs
  /// that share it, and the outputs they retain, share one limit.
  std::shared_ptr<native::MemoryBudget> native_budget;
  /// The VM's memory cap and instruction fuel; the whole run spends `setup_fuel`.
  BudgetLimits limits;
  /// What `decode` and `to_point_cloud` decode through, on the run's thread; null: both report "no image decoder".
  std::shared_ptr<const IImageDecoder> image_decoder;
  /// What `video:frame_at` decodes through, on the run's thread; null: it reports "no video frame source".
  std::shared_ptr<IVideoFrameSource> video_source;
  /// The result's stamp, the newest resolved input's: what annotations built for no image input, and a built cloud or
  /// voxel grid, are stamped with.
  Timestamp result_stamp = 0;
};

/// Runs `script` once on the calling thread's sandboxed state, in a fresh thread, as `chunk(inputs, params)`: `inputs`
/// is a table of `inputs` by name, `params` the JSON object `params_json` (empty means `{}`; at most `kMaxParamsDepth`
/// levels deep). An unavailable input is an error: the caller does not run a node whose input is unavailable (D25).
///
/// Script surface: objects are read-only views over their SDK field tables (lists are read element by
/// element; an int64 field is a `pj.int64`, exact past 2^53; a raw buffer is never exposed); a point
/// cloud also has `count`, `bounds`, `extreme_point`, `crop_box`, `filter`, `transform`, `xyz_points` and
/// `downsample(voxel_size)` (one centroid per occupied cube, an xyz-preset cloud), and image annotations `count` (the
/// points of every points annotation). An image or depth image has:
///   - `representation()`: how it is stored ("jpeg", "png", "raw:<encoding>", "compressed:<encoding>",
///     "depth:<encoding>").
///   - `decode()` -> decoded pixels, or `nil, reason`: its original samples (no display normalization), decoded
///     once per run and charged to the native budget for the rest of the run. Decoded pixels have `width`,
///     `height`, `channels`, `pixel(x, y)` (a number, or `{r, g, b[, a]}`), `to_gray()`, `crop(x, y, w, h)` (it
///     keeps the source's coordinates), `stats()` (`{min, max, mean}` over the finite samples) and
///     `threshold(op, value[, channel])` (`op` one of `<`, `<=`, `>`, `>=`, `==`, `~=`; `channel` 0-3 or r/g/b/a):
///     ImageAnnotations of the matching pixels, or `pj.unavailable(reason)` when they do not fit the budget.
///   - `to_point_cloud(camera_info[, {step, max_depth}])` -> an organized metric cloud in the camera's frame at the
///     image's stamp (16UC1/mono16 millimetres, 32FC1 metres), or `pj.unavailable(reason)`.
///   Annotations made from an image input pair with it: its store topic and its own stamp (0 is a stamp).
/// A video input (a packet, at or before the requested time in decode order) has `frame_at(t)` -> `{image,
/// presentation_ns}`, or `nil, reason`: the full frame presented at `t` (an integer number of nanoseconds or a
/// `pj.int64`; both on the dataset clock), decoded through `RunOptions::video_source` into an rgb8 image charged to the
/// native budget. Annotations made from a frame pair with the video's store topic at its presentation time; annotations
/// built for the video's topic pair with the frames decoded so far when they agree, else with the packet's own stamp.
/// A 3-vector is always an array `{x, y, z}`. `pj.unavailable(reason)` (the reason is cut
/// to 1 KiB), and `pj.tf`:
///   - `pj.tf.now`: the query instant, a `pj.int64`.
///   - `pj.tf.lookup(target, source[, when])` -> `{translation={x, y, z}, rotation={x, y, z, w},
///     target_frame}` or `nil, reason`. With no `when`, the entry clock at `pj.tf.now`; with a time
///     (number or `pj.int64`), the entry clock at that time; with an object, the header clock at the
///     object's own stamp: where its frame was when it was measured.
///   - `pj.tf.frames()`: every frame name.
/// Builders make the objects a script cannot read from its inputs. Both chain, and `finish()` returns the
/// object and spends the builder; every error names the field. A colour is `{r, g, b}` or `{r, g, b, a}`
/// (0-255, alpha 255 when absent); a diameter, size, thickness or font size is finite and greater than 0. A
/// script that retains a builder's object, or a view read from it, keeps its native bytes charged until it drops
/// them. A method reads its options table without invoking
/// metamethods.
///   - `pj.scene.new{frame_id, id}` -> `:sphere{center, diameter, color}`, `:cube{center, size, color}`,
///     `:text{position, text, font_size, color, billboard}`, `:line_strip{points, thickness, color}`: a
///     SceneEntities snapshot, a deletion of everything and then the entity if it has a primitive, so an
///     empty builder clears the topic (D27). A colour defaults to opaque white; a 3-vector is `{x, y, z}`.
///   - `pj.annotations.new{image_topic}` -> `:points{points, type, thickness, outline_color, fill_color}`
///     (`type`: points, line_list, line_strip, line_loop), `:circle{center, diameter, thickness,
///     outline_color, fill_color}`, `:text{position, text, font_size, text_color}`: pixel-space
///     ImageAnnotations over the image on `image_topic`, which is required. Naming an image input (by its store
///     topic or its input name) pairs them with it: its store topic, at its stamp; otherwise they keep
///     `image_topic` at `RunOptions::result_stamp`. A point is `{x, y}`; thickness,
///     font size and colours the script leaves out take the SDK's defaults. The SDK has no text background, so
///     an opaque `background_color` is an error.
///   - `pj.cloud.new{preset, frame_id}` -> `:add(x, y, z[, intensity | r, g, b])`, `:add_points{{...}, ...}`: a
///     PointCloud of PCL's familiar point types (`preset` "xyz", "xyzi" or "xyzrgb"; D14), every value finite and
///     within float32's range, r/g/b 0-255, packed into 16-byte records (native/point_cloud_builder.h).
///   - `pj.voxels.new{frame_id, origin, cell_size, dims}` -> `:set(i, j, k, value)` (1-based indices): a dense
///     VoxelGrid of one float32 `value` channel, every cell 0 until set (a finite value within float32's range), at
///     most 2^24 cells (D16). Its fields read as any object's; its cells are a raw buffer.
/// `math.random` is seeded the same way on every run, and there is no `os` and no clock (D2).
///
/// Binding the returned value: with no declared `outputs`, a non-table is one output `value`, a keyed
/// table is one output per key (sorted by name), and an array is named by type (`value`, `value_2`,
/// `text`, `cloud`, `scene`, `annotations`, `image`, `voxels`, `object`); an array may not hold `pj.unavailable`
/// (name the output instead), and booleans, functions, other values and objects of a type no output can
/// declare are errors. With declared outputs, a non-table binds to the single declared output, an array
/// binds by position and a keyed table by name; an undeclared key and a missing output are errors, and
/// every value must have its declared type (`nil` is missing; use `pj.unavailable`).
///
/// When a native operation fails on the native budget, a full garbage collection runs and the operation
/// is retried once, since the collector cannot see native bytes held by dead outputs.
[[nodiscard]] Expected<ScriptBundle, ScriptError> runOnDemandScript(
    const CompiledScript& script, Span<const NamedValue> inputs, std::string_view params_json,
    Span<const OutputSpec> outputs, const RunOptions& options = {});

}  // namespace PJ::scripting
