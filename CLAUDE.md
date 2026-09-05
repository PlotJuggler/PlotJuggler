# PlotJuggler 4 (PJ4)

## Goal

Build **PlotJuggler 4** from scratch as a modern desktop application that reaches parity-plus with PlotJuggler 3.x.

This is a greenfield app repo. It is not a refactor of PJ3. Code is cherry-picked from PJ3 and rebuilt on top of the `plotjuggler_sdk` foundation.

## License

PJ4 is **MPL-2.0** (see [`LICENSE`](./LICENSE)); every source file carries an
`// SPDX-License-Identifier: MPL-2.0` header. New source files must include that
header. The `plotjuggler_sdk` Conan package is a separate repo with its own license
(Apache-2.0 for `pj_base`/`pj_plugins`) — do not relicense it from here.

## Architecture

Full implementation plan: [`PJ4_PLAN.md`](./PJ4_PLAN.md). That document is the source of truth for module boundaries, delivery phases, and architectural decisions — read it before proposing structural changes.

Top-level layout (monorepo, per plan §0 and §5):

```
PJ4/
├── 3rdparty/                # vendored CMake deps + GPLv2/shareware compliance payload for the standalone raster_helper
├── pj_datastore/            # Level 0 columnar store + ObjectStore + DerivedEngine
├── pj_scene_common/         # backend-agnostic layered scene dock framework, shared by the scene widget families
├── pj_scene2D/              # 2D scene widget family: core logic, Qt widgets, tools, tests
├── pj_scene3D/              # 3D scene widget family: TF, pointclouds, occupancy grids, URDF/mesh, markers; HDR/SSAO/EDL pipeline + OpenGL widgets, demos
├── pj_marketplace/          # extension install/manage
├── pj_dialog_host/          # Qt host for plugin-provided dialogs
├── pj_scripting/            # Luau filter engine: self-describing filter classes for Data Processors
├── pj_runtime/              # services layer (Qt allowed, no Qt6::Widgets link)
├── pj_widgets/              # reusable Qt widgets and UI helpers
├── pj_plotting/             # Qwt plotting widget family: core adapters, Qt widgets, tests
├── pj_app/                  # main window shell
├── resources/               # SVG icons (ported from PJ3) + resources.qrc
└── PJ4_PLAN.md
```

The widget families (`pj_plotting`, `pj_scene2D/widgets` via the `pj_scene2d_widgets` target, and `pj_scene3D/widgets` via the `pj_scene3d_widgets` target) never depend on each other. Shared reusable Qt controls/helpers live in `pj_widgets` (including the reusable `Timeline` control — the per-source `display_offset` editor); shared runtime state flows through the `IDataWidget` contract exposed by `pj_runtime`.

### Placement rules

When adding files, use the owning module rather than creating new top-level folders. If the requested location does not match these boundaries, ask before proceeding and suggest the closest fit.

- `plotjuggler_sdk` (external, not in this tree): the plugin **SDK** (`pj_base`, `pj_plugins`), consumed as the `plotjuggler_sdk/<version>` Conan package pinned in `conanfile.txt` and linked as `plotjuggler_sdk::base` / `::plugin_sdk` / `::plugin_host`. Canonical object schemas (`Image`, `DepthImage`, `ImageAnnotations`, `PointCloud`, `FrameTransforms`) and their codecs live under its `pj_base/builtin/`. SDK changes happen in the [plotjuggler_sdk repo](https://github.com/PlotJuggler/plotjuggler_sdk), then a release + pin bump here.
- `pj_datastore/`: Level 0 columnar storage engine — `DataEngine` + `ObjectStore` + `DerivedEngine` + the host-side C-ABI write bridges. Also owns the **data-processor execution substrate** (`PJ::proc::DataProcessor` base + `ProcessorSisoAdapter`, which runs a processor as a `DerivedEngine` node, co-located here with the `ISISOTransform` interface it adapts to; the base stays Qt-free and Luau-free, so `pj_scripting` layers Luau on top). App-internal (plugins never link it; they reach storage through the `pj_base` C ABI). Pure C++20, no Qt; depends only on `pj_base`. Logic in `src/`, public headers in `include/pj_datastore/`, tests in `tests/`, docs in `docs/`. Licensed MPL-2.0.
- `pj_runtime/`: app runtime services and contracts: session/data lifecycle, catalog, playback, extension catalog, future workspace/transform/toolbox services. No concrete widgets and no `Qt6::Widgets` link.
- `pj_app/`: executable shell only: `MainWindow`, menus/toolbars/status bar, app dialogs, and wiring between runtime services and concrete widgets. Do not put reusable controls or business logic here.
- `pj_widgets/`: reusable Qt widgets and UI helpers that could be used by another Qt app. Depends only on Qt and the C++ standard library; no dependencies on `pj_runtime`, `pj_app`, or other PJ modules. Includes the reusable `Timeline` control (`Timeline.{h,cpp}`) — the multi-track Source Timeline that edits per-source `display_offset`: a Qt-free `TimelineScene` math class plus a runtime-agnostic `Timeline` QWidget, both plain-typed; the `SourceTimelineController` that binds it to `pj_runtime` lives in `pj_app`.
- `pj_scene_common/`: backend-agnostic layered scene dock framework (`scene_layer.h`, `layer_factory.h`, `scene_dock_widget.h`) shared by the 2D/3D scene widget families. Rendering-specific view state stays in those families, not here.
- `pj_plotting/`: Qwt plotting feature family. Put datastore adapters and plotting logic in `core/`, Qt/Qwt widgets in `widget/`, and focused tests in `tests/`.
- `pj_scene2D/`: 2D media/scene feature family. Put independent media logic in `core/`, Qt viewer widgets in `widgets/`, tests in `tests/`, and opt-in standalone dev utilities in `tools/` (gated by `PJ_BUILD_TOOLS`, off by default).
- `pj_marketplace/`: extension registry, download, install/manage services, and marketplace UI.
- `pj_dialog_host/`: Qt host/binding for plugin-provided dialogs. General app dialogs stay in `pj_app`; reusable dialog controls stay in `pj_widgets`.
- `pj_scripting/`: Luau filter engine for Data Processors — self-describing `.luau` filter classes behind a language-agnostic `ScriptEngine` seam (Python out-of-process is a future drop-in). It layers Luau onto the `PJ::proc::DataProcessor` base owned by `pj_datastore`. Do not place scripting code under `pj_app` or widget modules unless it is strictly UI/editor code.
- `pj_scene3D/`: 3D scene widget family (robotics viz): TF, pointclouds, occupancy grids, URDF/mesh, markers. Independent 3D logic in `core/`, OpenGL widgets in `widgets/`, tests in `tests/`, standalone dev demos in `demos/` (built whenever the `pj_scene3d_widgets` target exists, i.e. on a default build). Do not add 3D rendering code elsewhere.
- `resources/`: shared app resources registered in `resources.qrc`; module-local test/demo assets should live with that module.
- `3rdparty/`: vendored source dependencies added via CMake `add_subdirectory`, plus `3rdparty/retro/` — the GPLv2/shareware license + source-offer compliance artifacts shipped alongside the separately-licensed `pj-raster-helper` (the root `CMakeLists.txt` installs these next to the helper binary). Conan/system dependencies do not belong here.
- Top-level `raster_helper/` (the standalone GPL-2.0 `pj-raster-helper` executable that links vendored doomgeneric — PlotJuggler links none of it) and `raster_ipc/` (its header-only, Qt-free MPL-2.0 IPC contract, consumed by `pj_widgets`) are intentional non-`pj_` helper folders, not PJ modules, and are exempt from the no-new-top-level-folders rule.
- Packaging recipes live in one flat folder per artifact format, also exempt from that rule: `appimage/` (AppImage), `deb/` (Debian/Ubuntu package), `installer/` (Windows installer). They contain no compiled code — each holds the scripts, templates and metadata that repackage an already-built tree. A `.deb` recipe belongs in `deb/`, deliberately **not** `debian/`, which by convention marks a debhelper source package this repo is not.

## Documentation

Each PJ4 module owns its intent docs. An agent landing in the repo reads root `CLAUDE.md` → `PJ4_PLAN.md` → per-module `CLAUDE.md` → per-module `docs/` → code. If any link in that chain is missing or stale, treat it as a documentation bug, not a code bug.

### Code doc-comments

Every class, struct, function, and method — and any member that is not self-explanatory — carries a **concise doc-comment**, especially in **header files** (the API surface a reader meets first).

"Concise" is not "minimal": convey everything the reader needs and nothing they don't. Above all, document what the **name cannot convey** — pitfalls, side-effects, ownership/lifetime, threading or call-order constraints, units, and non-obvious invariants or rationale. Skip the genuinely self-evident (a trivial getter/setter, an obvious field): restating the signature in prose is just noise. **Comment the surprise, not the obvious.**

Further comment hygiene:

- **Keep comments focused on *why*, not *how*** — aim for self-explanatory code. If a comment is explaining mechanics the code already states, fix the code (rename, extract) instead of narrating it.
- **When refactoring, remove old comments** — never add comments about what changed. Comments describe the code as it is now, not its history.
- **Never document old behavior or behavior changes in code.** No "previously did X", "changed from Y", or migration notes in source — that belongs in commits/PRs, not comments.

### Per-module documentation contract

Every PJ4 module (anything matching `pj_*/`) owns:

- `<module>/CLAUDE.md` — one-paragraph purpose + pointer to the module's `docs/` and key headers. The entry point for an agent landing in that subtree.
- `<module>/docs/` — module-local intent docs. Minimum content depends on module complexity:
  - **Trivial / structurally obvious** (e.g. `pj_widgets`, `pj_dialog_host`, `pj_app`): CLAUDE.md only; no `docs/` required.
  - **Standard module**: `docs/REQUIREMENTS.md` (the WHAT).
  - **Module with non-obvious internals**: add `docs/ARCHITECTURE.md` (the HOW). See `pj_scene2D/docs/` for the reference shape.

Cross-cutting docs (porting strategy, glossary, ADRs) live in top-level `docs/`. Its scope is described in [`docs/README.md`](./docs/README.md). Notably, [`docs/QT_NOTES.md`](./docs/QT_NOTES.md) records the current Qt baseline from [`versions.env`](./versions.env) and what changed since 6.8 — read it before using an unfamiliar Qt API.

### Module documentation index

| Module | Entry point | Docs |
|---|---|---|
| `pj_app` | [pj_app/CLAUDE.md](./pj_app/CLAUDE.md) | — |
| `pj_runtime` | [pj_runtime/CLAUDE.md](./pj_runtime/CLAUDE.md) | — |
| `pj_widgets` | [pj_widgets/CLAUDE.md](./pj_widgets/CLAUDE.md) | — |
| `pj_plotting` | [pj_plotting/CLAUDE.md](./pj_plotting/CLAUDE.md) | — |
| `pj_dialog_host` | [pj_dialog_host/CLAUDE.md](./pj_dialog_host/CLAUDE.md) | — |
| `pj_scene_common` | [pj_scene_common/CLAUDE.md](./pj_scene_common/CLAUDE.md) | — |
| `pj_scene2D` | [pj_scene2D/CLAUDE.md](./pj_scene2D/CLAUDE.md) | [docs/](./pj_scene2D/docs/) — REQUIREMENTS, ARCHITECTURE, TECHNICAL_NOTES |
| `pj_marketplace` | [pj_marketplace/CLAUDE.md](./pj_marketplace/CLAUDE.md) | [docs/](./pj_marketplace/docs/) — REQUIREMENTS, ARCHITECTURE, USER_MANUAL, marketplace-spec |
| `pj_scene3D` | [pj_scene3D/CLAUDE.md](./pj_scene3D/CLAUDE.md) | [docs/](./pj_scene3D/docs/) — REQUIREMENTS, [ARCHITECTURE](./pj_scene3D/docs/ARCHITECTURE.md) (rendering pipeline, cameras, URDF/mesh) |
| `pj_datastore` | [pj_datastore/CLAUDE.md](./pj_datastore/CLAUDE.md) | [docs/](./pj_datastore/docs/) — REQUIREMENTS, ARCHITECTURE, USER_GUIDE, OBJECT_STORE_DESIGN |
| `pj_scripting` | [pj_scripting/CLAUDE.md](./pj_scripting/CLAUDE.md) | [docs/](./pj_scripting/docs/) — FILTER_CLASS |
| `plotjuggler_sdk` (Conan package) | [github.com/PlotJuggler/plotjuggler_sdk](https://github.com/PlotJuggler/plotjuggler_sdk) | the SDK repo owns its own `CLAUDE.md` + `docs/` tree |

### Freshness discipline

Before any commit that changes behavior, public APIs, ABI structs, module ownership, or user-facing semantics: verify that the relevant `CLAUDE.md` / `docs/` files still match reality. If they don't, either update them in the same change or ask whether to update before committing. Do not commit known-stale docs.

## Key sources

### `plotjuggler_sdk` (Conan package) — the plugin SDK, consumed as-is

`pj_base` (vocabulary types + canonical object schemas/codecs under `pj_base/builtin/`) and `pj_plugins` (extension ABI + runtime). What they are: see Placement rules; the full story lives in the SDK repo's own `CLAUDE.md`. Changes happen in that repo, not here. The recipe and prebuilt binaries live on PlotJuggler's JFrog Conan remote (anonymous read; `scripts/configure_conan_remote.sh` registers it, `build.sh` and CI call it). Build paths without Conan (WebAssembly, pixi) fall back to fetching the pinned release tag from GitHub and building the SDK in-tree (see the root `CMakeLists.txt`). To move to a new SDK: bump the pin in `conanfile.txt`, then `conan lock add --requires=plotjuggler_sdk/<v>#<rrev>` on `conan.lock`.

### `pj_datastore/` — build ordering

What it is: see Placement rules. Build-specific: `add_subdirectory(pj_datastore)` runs in the root `CMakeLists.txt` immediately after the SDK is resolved (so the `plotjuggler_sdk::base` / `pj_internal_fmt` targets it links already exist); its Conan deps (`nanoarrow`, `tsl-robin-map`, `benchmark`) live in the root `conanfile.txt`.

### The PJ3 reference tree (read-only)

PlotJuggler 3 source tree — [github.com/facontidavide/PlotJuggler](https://github.com/facontidavide/PlotJuggler). Point `PJ3_SOURCE_DIR` at a local checkout of it; there is no default location. This is **the primary source for cherry-picked code**. Expect heavy reference, particularly for:

- `plotjuggler_app/` — `PlotWidgetBase`, `PlotWidget`, `PlotDocker`, `TabbedPlotWidget`, zoomers, `AxisTimeOffset`, tracker, drag-drop, per-curve display transform UI, Lua engine
- `plotjuggler_base/` — shared base types (cross-check against `pj_base`)
- `plotjuggler_plugins/` — historical plugins (already ported; reference only)

Treat the PJ3 tree as read-only. Do not modify it from this repo.

The "wholesale lift" strategy for plot widgets (plan §5.3, §8) means porting files largely intact, then rebinding data reads (`PlotDataMapRef` → `DatastoreCurveAdapter` against `pj_datastore::DataReader`). Do not rebuild plot widgets from scratch.

## Build

- **Qt 6.11.1** (required; pinned by [`versions.env`](./versions.env)). Install via [`./install_qt6.sh`](./install_qt6.sh). See [`docs/QT_NOTES.md`](./docs/QT_NOTES.md) for what changed since 6.8 (new APIs past most training cutoffs, deprecations, build floors).
- **CMake + Conan**. CMake is the build driver; Conan provides external non-vendored dependencies.
- **C++20**.
- **Linux and Windows are both required shipped targets**; macOS is not yet. Keep the code portable — no Linux-only APIs or POSIX-specific paths in module code; gate anything platform-specific behind the usual CMake / `#ifdef` guards. Licensing note for Windows: conda-forge ships no LGPL FFmpeg for win-64, so pixi-based Windows artifacts must use the local `recipes/ffmpeg` package (the Conan path builds its own LGPL-trimmed FFmpeg on all platforms). Windows runtime packaging (windeployqt + bundling the FFmpeg/Qt DLLs `pj_app` needs) is in scope: the Conan `windows-ci.yml` resolves DLLs via `PATH` at test time, which is not a shippable layout.

### Vendored third-party

Mirror PJ3's `3rdparty/` convention. Vendored deps live at `./3rdparty/<name>/` and are added (most via `add_subdirectory`) from the top-level `CMakeLists.txt`.

Explicitly vendored (do not take from Conan or system packages):

- **Qt-Advanced-Docking-System** — docking framework used by `pj_app`.
- **nanocdr** — vendored via `add_subdirectory`.
- **doomgeneric** — a vendored C engine whose sources are globbed directly into the `pj-raster-helper` target (not `add_subdirectory`'d). It and `raster_helper` form an optional, GPL-isolated standalone executable that PlotJuggler never links. **Off by default**: the `pj-raster-helper` target is built only with `-DPJ_BUILD_RASTER_HELPER=ON` (and only when the vendored source is checked out), so an ordinary dev or CI build never compiles the doomgeneric engine. The one exception is the Linux release (`linux-appimage-release.yml`), which turns it on — via `PJ_BUILD_RASTER_HELPER=ON ./build.sh` — and stages the helper with `appimage/build_appimage.sh --retro-wad`, so the AppImage and the `.deb` ship it under `3rdparty/retro/` next to the app binary.

**Qwt is external, not vendored** (special case): `3rdparty/qwt/` holds only the
CMake glue — the default build `FetchContent`s the official 6.3.0 release tarball
(SHA-256 pinned) and compiles it unmodified; the pixi path links conda-forge's
prebuilt `qwt` via `-DPJ_SYSTEM_QWT=ON`. Never patch Qwt sources — PJ-specific
behavior that PJ3 carried as in-tree Qwt patches lives in the app instead: axis
tick-label formatting in `pj_plotting`'s `PlotScaleDraw` (twin in
`pj_dialog_host`'s chart preview), and the "Lines and Dots" curve style in
`pj_plotting`'s `PlotCurve` (Lines + one batched, pixel-weeded `drawDots` pass;
WASM keeps an explicit symbol for `PlotRhiCanvas` geometry instead). See
`3rdparty/qwt/README.PJ4.md`.

Other PJ3-style vendorables (`QCodeEditor`, `sol2`, `color_widgets`, `date`) will be vendored on the same pattern as we pull in the modules that need them — decide per-case when each module lands.

Everything else (GLM, assimp, FFmpeg, Lua runtime, etc.) comes from Conan — including
**backward-cpp** (consumed via `backward-cpp/1.6`, not vendored): `pj_app` links
`Backward::Backward` and installs a `backward::SignalHandling` crash handler in `main.cpp`
for symbolized stack traces. Its default `dw` backend pulls `elfutils` transitively from
Conan, so rich traces need no system `-dev` packages.

### Compile instructions

One-time setup (installs the Qt version from `versions.env` into `./.qt/`, ~1GB):

```bash
./install_qt6.sh
```

`versions.env` is the **single source of truth** for the PJ4 app version, Qt
version, and AppImage arch. `install_qt6.sh`, `build.sh`, `run.sh`, Docker, CMake,
and CI all consume it. To upgrade Qt, update `PJ_QT_VERSION` there; Windows CI
still installs Qt inline because the Linux installer uses the `gcc_64` build.

> **Versioning rule — never hardcode a version.** `PJ_APP_VERSION` (app version),
> `PJ_QT_VERSION` (Qt), and `PJ_APPIMAGE_ARCH` (AppImage arch) live **only** in
> [`versions.env`](./versions.env); always read them from there rather than
> writing a literal (`3.999.0`, `6.11.1`, `x86_64`) anywhere. How each consumer
> reads it: bash scripts `source "<anchor>/versions.env"` by absolute path; CMake
> via `pj_read_versions()` in [`cmake/PjVersions.cmake`](./cmake/PjVersions.cmake)
> (which stamps `cmake/pj_version.h.in` → `main.cpp` and feeds `find_package(Qt6 …)`);
> CI via a `Read versions` step (`source ./versions.env` → `$GITHUB_ENV`); Docker
> via `--build-arg` from `build_in_docker.sh`. To change a version, edit
> `versions.env` and nothing else. **Only the app version is overridable** per
> build — `-DPJ_VERSION=<v>` or the `PJ_VERSION` env var (release CI stamps the git
> tag); `PJ_QT_VERSION`/`PJ_APPIMAGE_ARCH` are hard pins with no override. The Qt
> platform dir tokens (`gcc_64`/`msvc2022_64`) are a separate namespace — leave
> them literal, never derive them from `PJ_APPIMAGE_ARCH`.

System build dependencies (Linux): the Conan FFmpeg build needs `libva-dev` and
`libdrm-dev` on the build host, because `conanfile.txt` enables
`ffmpeg/*:with_vaapi=True` / `with_libdrm=True` for VAAPI hardware video decode
(the `FfmpegDecoder` GPU path; it falls back to software when no usable GPU
driver is present). The recipe's `vaapi/system` + `libdrm` wrappers resolve
`libva` / `libva-drm` via `pkg-config`, so these `-dev` packages must exist at
build time:

```bash
sudo apt-get install libva-dev libdrm-dev
```

End users only need the ubiquitous `libva2` runtime (packaging pulls it in). CI
installs these in `.github/workflows/linux-ci.yml`.

Build (configures Conan, runs CMake, builds):

```bash
./build.sh
```

That script:

1. Checks for `.qt/<PJ_QT_VERSION>/gcc_64/` and tells you to run `./install_qt6.sh` if missing.
2. Runs `conan install ... --output-folder=build --build=missing -s compiler.cppstd=20` (reads `conanfile.txt`).
3. Configures CMake with `CMAKE_TOOLCHAIN_FILE=build/conan_toolchain.cmake` and `CMAKE_PREFIX_PATH=./.qt/<PJ_QT_VERSION>/gcc_64`.
4. Builds with `cmake --build build -j$(nproc)`.

Run the app:

```bash
./run.sh
```

`run.sh` unsets `QT_IM_MODULE` before launching. Otherwise the IBus platform input context can get loaded from a system / older Qt install and crash under the pinned Qt runtime.

Run the tests (`enable_testing()` is wired at the top level, so `ctest` covers every module):

```bash
ctest --test-dir build --output-on-failure
```

Re-running `./build.sh` after code changes does incremental builds. `ccache` is picked up automatically if installed.

After a rebuild meant to pick up a C++ change, confirm the file actually recompiled (grep the build log for `Building .../<file>.cpp.o`, or check the `.o` mtime) before claiming the fix is live — RCC/QSS rebuilds can mask a stalled C++ recompile, so an edit looks like it had "no effect" when it was never compiled. Never chain `pkill … ; ./build.sh`: the chained build exits 144 and skips recompiling. Kill the running app in its own command, then run `./build.sh` standalone.

Submodules (`3rdparty/`): `git submodule update --init --recursive` on first clone.

Worktrees: use the **`./worktree-new.sh`** / **`./worktree-rm.sh`** helpers at the
repo root rather than hand-rolling `git worktree`:

```bash
./worktree-new.sh fix/foo            # .worktrees/foo, branched off origin/main
./worktree-rm.sh  foo                # after the PR merges (deletes the branch too)
```

They handle the two things the manual path keeps getting wrong: the ~1GB Qt
install lives only in the primary checkout (`.qt/`), so a fresh worktree needs a
`.qt` symlink to it by **absolute** path; and the `3rdparty/` submodules are initialized by borrowing the primary
checkout's objects (`--reference`, local + offline) instead of re-cloning from
GitHub. Set-up-only by default (seconds); pass `--build` to compile. The scripts
resolve the primary checkout themselves, so they work from any worktree.

## UI conventions

- **Prefer `.ui` files over programmatic widget construction.** Widgets, layouts, menus, toolbars, dialogs — build them in Qt Designer (`.ui`) and load via `uic`. Use `AUTOUIC` in the module's `CMakeLists.txt`. Drop to hand-written `QWidget` subclasses only when the construction is genuinely dynamic (e.g. widgets created at runtime from plugin metadata) or when I explicitly ask for it.

## v1 scope (per plan §0)

Parity-plus with PJ3: file + streaming sources, 11 built-in transforms, undo/redo, derived-series editor (incl. Lua via `pj_scripting`), reactive scripts (via Toolbox + `onTimeChanged`), multi-tab workspace, marketplace install UI, all toolboxes.

The 3D widget family ships as `pj_scene3D` (built and wired into `pj_app` via `Scene3DDockWidget`): TF, pointclouds, occupancy grids, axis/grid render passes, SceneEntities/markers, pluggable camera models, URDF/mesh robot models, the HDR/tonemap/SSAO/EDL rendering pipeline, live/streaming TF+object ingest (`TransformService` + `driveVisibleLayersToLiveEdge`), and per-use parser bindings (`parse_locked.h`) — see `pj_scene3D/docs/ARCHITECTURE.md` for the as-built design, `docs/REQUIREMENTS.md` + plan §5.5 for scope.

For WebAssembly, Scene3D is a compile-time platform split behind those same
public widget names. Desktop remains the full `QOpenGLWidget`/OpenGL layer graph;
the browser uses a minimal `QRhiWidget` product backend and must add object
layers one work package at a time. Do not pull native GL passes, Assimp/network
model code, or codecs into the browser merely to expose grid/TF. Browser shader
packs are GLSL ES 300 and must be reproducibly rebuilt with the pinned host
`qsb` and hash-checked at configure time. Every new static archive in the
monolithic WASM executable also needs an ABI/process-global-symbol collision
audit; a successful link alone did not catch the prior Qt/libjpeg collision.

Deterministic browser fixtures must reproduce both bytes and the product state
needed by the UI. In particular, the global Timeline prefers scalar/object
bounds; multiple TF samples do not widen it when another object topic provides
a single timestamp. Scene3D seek fixtures therefore need at least two relevant
object-bound samples, not just a multi-sample TF buffer.

## Non-goals (explicitly deferred)

- `StatePublisher` parity
- Exact 3.x UI/terminology parity
- Hot reload of running extension instances
- Full backward compatibility with 3.x layout files
- Recreating the removed prototype app as the final app

## Workflow notes

- Architectural questions → consult `PJ4_PLAN.md` first; escalate if the plan is silent or contradictory.
- New modules must respect the dependency rules in plan §5.
- Keep host code (`pj_app`, `pj_runtime`, `conanfile.txt`) **domain-neutral**: no plugin-specific terms (dataset-domain names, "episode") or hardcoded per-plugin policy. Mechanisms/protocols stay generic; the plugin supplies the domain meaning. A genuinely general capability motivated by one plugin is fine (e.g. a standard codec); a plugin-specific reference or branch in the host is not.
- Delegating to Codex: invoke it **harness-tracked with a watchdog** (`codex exec` inside a backgrounded, timeout-bounded `Bash`) — the companion job model sends no completion notification and Codex can hang silently. Judge liveness by job-log mtime, not the "running" status; on timeout, cancel and take over the build/test yourself. Codex output is usually high quality when it does finish — act on its findings.

### Execution autonomy

Once we've agreed on an approach, run the plan to completion — work through implementation, build, and tests **without pausing for "should I proceed?" check-ins**. The User reviews the final result, not each step; don't ask for mid-task confirmation on reversible work. "Run to completion" stops **at the commit boundary, not through it**: commits (see Commit policy), pushes/PRs, and anything outward-facing or destructive still require a stop-and-surface.

### Verifying visual / performance changes

Don't just assert a change works and hand it back ("I don't see any difference" / "are you sure something changed?" are recurring round-trips). Calibrate the claim to what you actually checked:

- **Visual / widget / layout changes** — self-verification is often impractical, so this is a *suggestion*, not a rule: when it's cheap, a screenshot (qt-widgeteer harness or `/run`) is worth it; otherwise just be honest about what you did and didn't confirm rather than implying it's visually verified.
- **Performance changes** — these *are* measurable, so prefer a **number** over assertion (replot/repaint rate, which should stay ≤60 Hz; frame time; a profiler counter).

### Commit policy

- **Never commit autonomously.** Surface the diff, then ask for approval. Commit only after explicit user confirmation in that turn.

### Git gotchas (these cost time repeatedly)

- After `git mv` + edits to the moved file, `git add` the new path — the edits are NOT staged with the rename ("100% similarity" in commit output is the tell that you shipped a stale blob).
- `git rebase` skips pre-commit hooks. After any rebase with **manual conflict resolution**, run `pre-commit run --all-files` and fold fixes into the originating commit (`--fixup` + `--autosquash`) so every commit stays hook-clean.
- After a squash-merge, verify every intended commit landed (`git log --oneline <merge-commit>`); commits pushed after the squash diff was prepared get silently dropped.
- After merging `origin/main` into a feature branch, grep that the feature's entry points still have **production callers** (not just defs/tests) — a modify/delete conflict can accept the deletion and silently drop wiring while CI stays green.

### Porting policy from PJ3

- **Default: port, don't rewrite.** For every UI element, widget, or helper we need, check the PJ3 tree at `${PJ3_SOURCE_DIR}` first. If PJ3 has something that works, port it. Greenfield rewrites need a real reason.
- **Style changes are expected; widget names are not.** When porting, adapt file/class names and member conventions to plotjuggler_sdk style (`PascalCase.{h,cpp}`, `PJ::` namespace, `trailing_underscore_` members, Google C++ / 2-space / 120-col). But **preserve the `objectName` of widgets inside `.ui` files verbatim** (e.g. `buttonLoadDatafile`, `frameFile`, `checkBoxAddPrefix`, `displayTime`, `playbackLoop`, `streamingSpinBox`) so existing layout files, stylesheet selectors, and user muscle memory keep working — unless I explicitly ask you to rename one.
- **Rebind data paths.** PJ3 wiring into `PlotDataMapRef` / `TransformsMap` becomes wiring into `pj_runtime` services (`CatalogModel`, `SessionManager`, `PlaybackEngine`, `TransformRegistry`). That's the one systematic rewrite.
- **Proactively surface improvement opportunities.** If you see a chance to improve separation of concerns, reusability, testability, or remove duplication while you're porting — flag it and **ask for approval before changing**. Don't silently refactor, and don't silently skip obvious wins. The bar is: "is there a cleaner shape that we'd regret not taking?" If yes, ask.
- **Don't fix what isn't broken.** Code that already reads cleanly and does the right thing gets ported close to verbatim (modulo style). Save the refactor energy for real problems.
