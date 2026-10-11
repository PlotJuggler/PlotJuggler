# Derived objects: requirements

**Status.** These are the agreed requirements (decisions D1-D30). They were revised
on 2026-10-10 with evidence from the functional prototype (pull requests #697, #698 and
#699). A decision changes when evidence shows a better approach; the change is recorded
here explicitly, with its reason. [ARCHITECTURE.md](ARCHITECTURE.md) describes the target
design. [SCENARIOS.md](SCENARIOS.md) lists the behaviours and corner cases that every
implementation step must keep. [CARVING.md](CARVING.md) tracks the delivery. The
discovery material (user stories US1-US12, candidate requirements C1-C12, the planning
notebook and the review notes) lives on the `planning/lua-derived-object-dag` branch, in
`docs/`.

## Purpose

Make transformation of 3D data quick and intuitive. Users type or reuse functions in Lua
(Luau) or Python, use internal helpers and libraries for common operations, or ask the AI
agent. Functions consume canonical SDK objects and scalar or string values, and produce
named, typed results on demand. Examples: filtering a point cloud, merging clouds,
producing a dense voxel grid, measuring a cloud and showing the measurement as 3D markers.

The first release focuses on 3D scene objects. Scene2D objects were investigated so that
supporting them later is an incremental extension, without a deep architectural refactor.
This does not commit the first release to complete image or video processing.

The feature is a **stateless, lazy dependency graph**. A scene, another function or an AI
query can request a result. A function's existence and evaluation do not depend on a scene
being open. Object outputs appear in the same tree as custom derived time series, and can be
dragged into an ordinary compatible 3D view.

It is also a platform capability, not a feature of one plugin. Any client of the plugin ABI
(the Transform Editor, the Assistant Agent, the Anomaly Detector, a plugin that does not
exist yet) can declare a function over any mix of scalar and object inputs, get its typed
results, and have object outputs appear as ordinary topics.

Outcomes the feature serves:
- people adapt and inspect recorded or live data inside their PJ4 workspace, without
  exporting it to another tool or writing a plugin;
- the AI computes answers from actual data and presents spatial evidence for them that a
  person can inspect;
- people reuse analysis functions and compose results across investigations.

Authoring direction: reusable operations are Lua or Python functions called from scripts. A
dedicated Filters-style UI is not required.

## Agreed behavior

The IDs below identify decisions for subsequent discussion, not implementation tasks.

| ID | Decision |
|---|---|
| D1 | Evaluate lazily at a requested time. Inputs may themselves require lazy loading/decoding. Do not process the entire recording merely to create a node. |
| D2 | Functions are stateless across evaluations in v1. Visiting times in a different order must not change the result for the same dependencies and parameters. |
| D3 | Maintain a proper dependency DAG. Named outputs can feed downstream nodes, including when nobody is displaying the intermediate objects. |
| D4 | Declare every required source binding before execution. Support arbitrary input counts and mixtures of canonical objects, numeric timeseries, string timeseries and compatible upstream results. Hidden runtime topic discovery inside Lua is outside this contract. |
| D5 | Resolve each source independently using its closest sample **at or before the requested scene/query time**. There is no primary input whose timestamp becomes the clock for the other inputs. |
| D6 | The executor calls Lua only after **all required inputs** have been found. Missing input means no result: clear the affected display and identify the missing source. Do not present the previous geometry as a valid current result. |
| D7 | A node belongs to exactly one dataset, with one time domain and that dataset's TF buffer. Mixing data from two datasets in one function is not an intended feature, not a deferred one (revised 2026-10-10). A recipe whose inputs span datasets is rejected. Comparing recordings works as it always has in PlotJuggler: one series per dataset on the same plot. What is not supported is one derived function reading from two datasets (wording added 2026-10-13). |
| D8 | Configure output names upfront, consistent with the named timeseries-transform workflow. The working agreement is also to declare each output's fixed type upfront. Exact metadata syntax is not frozen. |
| D9 | Support SISO, MISO and MIMO with independent input/output types. A function can return multiple canonical objects, numbers and strings in one evaluation. |
| D10 | Numeric and string outputs are queryable results and can feed this DAG. A numeric output of a function with object inputs can also be a plottable series: one value per entry of its object inputs over the history (its sampling clock is the union of those inputs' entry timestamps), recomputed when the function or its inputs change, and computed only while the series is displayed or explicitly requested (D1). Which engine computes the series is an implementation choice. Revised 2026-10-10 from "no plottable series in v1", on the evidence of the prototype's series mode and the need for per-entry measures over a recording. |
| D11 | Lua implements the algorithm and its math. Native bindings provide safe object access/construction and supporting geometry operations. |
| D12 | Expose a read-only TF lookup capability to the script. The script chooses transforms and the output frame; the host does not impose an output frame or automatically align inputs before the call. A small geometry helper library is desired. |
| D13 | Point clouds need native point access helpers; authors must not unpack canonical `data[]` manually. |
| D14 | Provide familiar **XYZ, XYZI and XYZRGB** presets for constructing clouds. Do not require Lua authors to define arbitrary field layouts. This is an ergonomic reference to PCL, not a decision to add PCL as a dependency. |
| D15 | Filtering must be able to copy complete point records into a compatible output, preserving intensity, RGB, ring and unknown extra fields. Preset construction and a preserve-input-layout copy path are both needed. |
| D16 | Support both a voxel-decimated **PointCloud** and a real canonical **dense VoxelGrid**. They are different output types, not interchangeable representations. No particular voxelization algorithm was selected. |
| D17 | Time-dependent procedural functions can opt into an explicit time input. That input participates in caching. Without it, movement of the cursor alone need not rerun a function when its resolved dependencies remain unchanged. |
| D18 | Deliver the Lua feature on **desktop and browser/WASM in v1**. Repository desktop targets include Linux and Windows. |
| D19 | An AI agent can create or reuse functions and explicitly trigger execution at a specified timestamp, independently of the current scene cursor and without rendering. It can read scalar/string results and arrange for object outputs to be displayed. Inspector findings commonly include 3D markers. |
| D20 | Quick, intuitive 3D transformation is the product goal. Support typed/reused functions, internal helpers and Lua libraries for common operations, and AI-assisted use. The initial library catalogue remains open. |
| D21 | Common operations can be reusable Lua functions called directly from hand-written or AI-generated scripts. A dedicated Filters-style chooser and generated parameter forms are not required. Simplicity should come from the functions/helpers; this does not remove the agreed source bindings, output identities or ordinary scene integration. |
| D22 | Evaluation at T and displaying its results do not automatically move the timeline cursor to T. The AI can move the cursor through a separate API action. Evaluation, result display and cursor navigation are independent operations. |
| D23 | A layout may optionally contain a copy of the custom function, controlled by the existing layout option. If it does not contain a copy and the referenced function changes, different results are an accepted known limitation. Mandatory function-revision retention and an explicit version-upgrade workflow are not requirements. |
| D24 | V1 focuses on 3D scene objects, but requirements discovery must also reason about Scene2D objects and mixed 2D/3D workflows. Later Scene2D support must be incremental and must not require a deep architectural refactoring. Exact additional v1 type coverage is still open. |
| D25 | An evaluation can explicitly mark an output unavailable while returning useful values on other outputs. For a cloud with no valid points, maximum Z is unavailable, the report can explain why, and markers can be a valid empty result. Downstream functions requiring the unavailable output do not run for that evaluation; functions requiring only available outputs remain eligible. |
| D26 | A displayed AI finding remains visible through subsequent timeline scrubbing until explicitly removed or replaced, labelled with its computation time. This settles lifetime within the current session, using a marker already expressed in a fixed map frame as the example. Layout persistence and moving-frame pose behavior remain separate questions. |
| D27 | A live derived `SceneEntities` node publishes a complete snapshot on every evaluation. An entity id present in one evaluation and absent from the next disappears automatically; there is no deletion/expiry event to author or replay. This governs the node's current rendered state and is distinct from a separately retained AI finding under D26. |
| D28 | The smallest viable v1 walkthrough is US1 (crop a region, preserving fields) plus US5 (measurement and visible witness from one evaluation). The remaining core-candidate stories (US2-US4, US6-US11) are extensions to prioritize once that pair works end-to-end; US12 (camera projection) is deferred out of v1 entirely, retained only as the architectural acceptance example for D24. |
| D29 | Lua (Luau) and Python are both desired languages for authoring functions. The decisions above are language-neutral: where they say "Lua", read "the function's language". In particular D2 (stateless across evaluations) and D6 (run only when all required inputs exist) bind Python exactly as they bind Lua. |
| D30 | A function slower than its input keeps updating at the rate it can sustain: continuous playback or streaming must never stop it from showing results. While a newer result is being computed, the previous result stays visible and the scene says how far it lags behind the cursor. The indicator disappears once the shown result belongs to the cursor's time, and once the cursor stops the display always converges to that result. The indicator also tells the person watching that the function is too slow for the current rate and needs attention. Missing input and errors still clear the display (D6). |

For D5, suppose a cloud has a sample at 10.00 s and a threshold has samples at
9.90 s and 10.05 s. A request at 10.10 s uses the cloud at 10.00 and the threshold
at 10.05. The threshold change can therefore change the output while the cloud
remains the same. Upstream derived nodes are also requested at the query time;
their cached evaluations are not a substitute sampled history to search.

For D17, “place a marker at `sin(t)`” needs an explicit time dependency.
“Find the highest point of the selected cloud” normally depends on the cloud
identity, parameters and any TF used, rather than every cursor tick. Input sample
timestamps are metadata distinct from opting into continuously changing query time.

For D19, the AI can request a measurement at 12.4 s while the scene cursor is at
18 s. The invocation resolves inputs for 12.4 s and leaves the cursor unchanged.
Its returned values describe that evaluation. A scene
requesting a derived output at another time is a separate evaluation request.
Showing the result also leaves the cursor unchanged (D22). The AI may explicitly
invoke a separate cursor-control API if navigation is wanted. D26 settles that a
displayed finding survives later scrubbing and identifies its computation time.
Moving-frame behavior, precise presentation and durable persistence remain open.
Displaying a retained finding and displaying a derived topic evaluated at the
current cursor are distinct consumers of the shared evaluation mechanism.
The time label identifies the requested data time (12.4 s in this example), not
the wall-clock time at which the computation happened.

For D25, an unavailable measurement is distinct from zero, a valid empty object,
a missing required source and an execution error. Availability belongs to each
declared output; its declared type stays fixed. Return a consistent evaluation
bundle containing the available values and explicit unavailable statuses. Do not
substitute a previous measurement. The Lua spelling and SDK representation remain
design choices; this decision does not authorize silently omitting declared outputs.

For D27, suppose a diagnostic function draws a corridor box and a mode label at
T1, but only the box at T2 because the mode string became unavailable. The label
disappears at T2 without any explicit delete call; a fresh evaluation back at T1
would draw it again. This keeps a live derived node's presentation consistent
with stateless, arbitrary-time seeking (D2) and avoids reintroducing history
replay through marker events. It does not change how a separately retained AI
finding behaves once displayed (D26); that finding keeps its own evaluated
evidence regardless of what the live node currently publishes.

## Representative workflows

| User intent | Inputs | Outputs | What it establishes |
|---|---|---|---|
| Crop a cloud to a box | PointCloud; configured box parameters | PointCloud | Easy point access and record copying preserve extra fields. |
| Change cropping using recorded telemetry | PointCloud + closest earlier numeric threshold | PointCloud | Mixed inputs, with no primary clock. |
| Annotate using a recorded state | PointCloud + closest earlier string state | SceneEntities | String input is a real value, not a numeric coercion. |
| Merge two sensors | Two PointClouds; dataset TF lookup; chosen output frame | PointCloud | Different acquisition times/frames; Lua owns alignment and merge policy. |
| Downsample into centroids | PointCloud + voxel-size parameter | PointCloud | PointCloud presets can construct new geometry. |
| Create occupied cells | PointCloud + grid parameters | VoxelGrid | Canonical output type can differ completely from input type. |
| Inspect Z extent | PointCloud; chosen frame | Number(s), string, optionally SceneEntities | Headless measurement and visible evidence can be outputs of one call. |
| Animate a diagnostic marker | Explicit time input; any declared data inputs | SceneEntities | Procedural time is an intentional dependency. |
| Reuse a derived result | Output of another node plus other declared inputs | Any declared supported output type | The graph exists independently of rendering. |

The “maximum range on Z” example exposed an important correction: scalar/string
outputs must be possible. The precise measurement should distinguish **maximum Z**
from **Z span (`max_z - min_z`)**, with the frame and units explained to the person reading it.

## Scene2D extension constraint

D24 is confirmed. The following are **proposed checks of that constraint**, not
additional accepted v1 algorithms or bindings. Detailed examples are in
the Scene2D discovery probes (`lua_3d_user_stories.md` on the planning branch).

| Future workflow | What the initial architecture must leave possible |
|---|---|
| Image + scalar threshold → Image + count | Add pixel access/builders and Lua helpers to the same typed function model; scalar changes invalidate results even when the image sample is unchanged. |
| Image + recorded state → ImageAnnotations + report | Produce genuine pixel-space annotations with a target image, without requiring a 3D frame, TF lookup or open viewer. |
| Image + annotations + CameraInfo → cropped image + matching annotations/calibration | Preserve relationships between outputs of one MIMO evaluation; distinguish image coordinates from world coordinates. |
| Depth image + CameraInfo → PointCloud + ImageAnnotations + measurement | Mix 2D and 3D types in one DAG and one AI invocation; route outputs independently to compatible viewers. |
| Video → decoded image measurement at T | Allow source-specific resolution/decoding with a stable result identity; keep algorithm statefulness separate from codec implementation state. |

The proposed shared contract covers named typed values, declared dependencies,
query time, immutable evaluation results, cache invalidation and explicit-time AI
execution. Object access/construction, decoding and viewer adaptation remain
specific to the supported type. Extending the existing canonical type handling is
sufficient; a new general-purpose type/plugin framework is not required by D24.

Scene2D adds questions that should not be hidden by a universal “scene object” API:
raw versus rectified pixels; image encoding/depth units; annotation target and
coordinate space; and compressed-video presentation time versus packet storage
time. In particular, an image has a coordinate frame but an image annotation uses
pixel coordinates; neither should inherit mandatory 3D pose semantics.

Recommended separation: analysis consumes defined data representations, independent
of viewer zoom, colormap, rectification settings or preview quality. Explicit image
conversion/rectification functions may use declared calibration inputs. Decoders
can be reused without turning the viewer's pending-frame mailbox into the DAG's
result API. Missing data, pending work and a valid empty result retain their own
meanings across both scene families.

## Deferred scope

- Persistent accumulated maps, state carried between calls, incremental replay and
  checkpointing. These would add history-dependent semantics to arbitrary seeking.
- Bounded history-window inputs. They could remain pure and lazy, but require a
  separate range-selection, invalidation and memory-budget contract.
- Sampled histories of string outputs, and series sampled on a clock other than the
  entries of the object inputs (D10 covers numeric outputs on that clock).
- Arbitrary user-authored binary point-field schemas in Lua.

Python is not deferred: D29 makes Lua (Luau) and Python both desired languages.

## Still open

These are not accepted requirements. Resolve them through the continuing
discussion. The planning notebook and the review notes, on the planning branch, give recommendations and examples.

| Topic | Decision still needed |
|---|---|
| Story validation and success criteria — partly settled | D28 settles v1's minimum scope: US1 + US5 as the required walkthrough, US2-US4/US6-US11 as prioritized extensions, US12 deferred entirely. Essential outcomes and responsiveness expectations for that walkthrough remain open. |
| Inspector presentation — partly settled | D26 settles session lifetime and computation-time labelling. Define moving-frame behavior, exact time-label presentation, replacement/ownership and layout persistence; do not reopen retention through scrubbing or automatic cursor movement. |
| Unavailable-output representation | D25 settles per-output absence and downstream gating. The exact Lua syntax, SDK status/reason representation and presentation remain to be specified. |
| Function authoring | Exact function signature, declared aliases/types, instance bindings, parameter representation and reusable script/library packaging. No dedicated Filters-style UI is required. |
| Python specifics | D29 settles that Python is wanted, and D18 applies to it: Python in the browser is a goal, gated on feasibility. The candidate path is CPython 3.14's tier-3 Emscripten target, statically linked into the threaded WebAssembly build (static linking is the only supported embedding; a pthreads build is possible and the deployment already serves the required isolation headers). Until a spike proves it, the prototype runs Python on desktop only. Still to decide: the trust model (an in-process interpreter is a convenience for trusted scripts, not a security boundary; isolation would need a separate process), how D2 is enforced when module state survives in one long-lived interpreter, acceptable throughput while the GIL serializes Python functions, and Windows packaging. |
| Cloud details | Index convention, supported coordinate/attribute encodings, organized-cloud filtering, preset conversion and heterogeneous merge policy. |
| Voxel details | Builder/access API, supported cell presets, dimensions/origin convention and allocation limits. |
| Timing details | Sample timestamp exposure, output timestamp policy, optional staleness limits, TF lookup time and display/raw-time conversion boundary. The TF clock question (header stamps against the dataset's entry clock) is settled by two timestamps per sample (ARCHITECTURE.md, TF R6). |
| API coverage | Exact initial builtin binding set, compressed-cloud normalization and zero-source functions. Required examples do not imply complete bindings for every builtin in the first release. D24 requires incremental Scene2D extension, not full 2D coverage in v1. |
| Future 2D semantics | Pixel representations and access presets, raw/rectified coordinates, annotation/image pairing, calibration updates and video presentation-time selection. Record constraints now; defer detailed APIs until the corresponding stories enter scope. |
| User/AI presentation | The prototype implements attach, remove and focus as scene tabs in `pj.plot_tabs.v1` (SDK 0.36.0). Still open: result ownership and replacement, provenance, persistence and undo behaviour. |
| Implementation/performance | D30 settles scheduling and cancellation. The prototype writes results into the ObjectStore; a pull design (layers read from the evaluator) is proposed, pending a spike (ARCHITECTURE.md). One global byte budget bounds all on-demand results (ARCHITECTURE.md §9). Still open: native allocation accounting and representative workload targets. |

The prototype's measurements (GUI-thread freeze, evaluation cost, streaming plateau) are
summarized in ARCHITECTURE.md. No delivery-time estimate exists.

## How this document changes

Accepted behaviour lives here; the design lives in ARCHITECTURE.md. A decision changes when
evidence shows a better approach: record the new wording, the date and the evidence in the
decision's row. Do not silently turn a recommendation into an agreement.
