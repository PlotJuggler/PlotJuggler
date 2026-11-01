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

Module boundaries and dependency rules are below; each module's `CLAUDE.md` and `docs/` describe its implementation. Cross-module designs: derived series and objects in [docs/derived_objects/](./docs/derived_objects/ARCHITECTURE.md), recording and source cache in [RECORDING_AND_CACHING.md](./docs/RECORDING_AND_CACHING.md) and [session_recorder_design.md](./docs/session_recorder_design.md). [docs/archive/](./docs/archive) holds retired plans (history only).

```
PJ4/
├── 3rdparty/          # vendored deps + raster-helper compliance payload
├── pj_datastore/      # Level 0 columnar store, ObjectStore, IncrementalEngine
├── pj_tf/             # Qt-free TF frame tree
├── pj_scene_common/   # layered scene dock framework (2D/3D)
├── pj_scene2D/        # 2D scene widget family
├── pj_scene3D/        # 3D scene widget family, QRhi renderer
├── pj_marketplace/    # extension install/manage
├── pj_dialog_host/    # Qt host for plugin-provided dialogs
├── pj_scripting/      # Luau and embedded-Python engines
├── pj_runtime/        # services layer (no Qt6::Widgets)
├── pj_widgets/        # reusable Qt widgets and UI helpers
├── pj_plotting/       # Qwt plotting widget family
├── pj_custom_widgets/ # Custom Widget panel: a dock hosting a Widget plugin's Qt widget
├── pj_app/            # main window shell
├── resources/         # SVG icons + resources.qrc; wasm-presets/ browser sample presets
├── raster/            # helper/ executable + ipc/ wire contract
├── scripts/           # setup, worktrees, pre-commit, windows/ entry points
├── packaging/         # Nix, conda, AppImage, Debian package, Windows installer, hosted browser app
└── docs/              # cross-cutting guides, research, archived plans
```

The widget families (`pj_plotting`, `pj_custom_widgets`, `pj_scene2D/widgets` via the `pj_scene2d_widgets` target, and `pj_scene3D/widgets` via the `pj_scene3d_widgets` target) never depend on each other. Shared reusable Qt controls/helpers live in `pj_widgets`; shared runtime state flows through the `IDataWidget` contract exposed by `pj_runtime`.

### Placement rules

When adding files, use the owning module rather than creating new top-level folders. If the requested location does not match these boundaries, ask before proceeding and suggest the closest fit.

- `plotjuggler_sdk` (external): the plugin SDK (`pj_base`, `pj_plugins`; canonical object schemas and codecs under `pj_base/builtin/`), linked as `plotjuggler_sdk::base` / `::plugin_sdk` / `::plugin_host`. Change it in the [SDK repo](https://github.com/PlotJuggler/plotjuggler_sdk), then release + bump the pin here.
- `pj_datastore/`: columnar `DataEngine`, `ObjectStore`, `IncrementalEngine`, host-side C-ABI write bridges, and the `PJ::proc::DataProcessor` substrate, whose base stays Qt-free and Luau-free (`pj_scripting` layers the engines on top). Pure C++20, depends only on `pj_base`; app-internal (plugins reach it through the C ABI).
- `pj_tf/`: TF frame tree, a static Qt-free leaf (only `glm`, `plotjuggler_sdk::base`, `nlohmann_json`). Ownership, ingest and signals stay in `pj_runtime`; rendering in `pj_scene3D`.
- `pj_runtime/`: runtime services and contracts (session, catalog, playback, extensions, TF, derived engines). No concrete widgets, no `Qt6::Widgets` link.
- `pj_app/`: executable shell only (`MainWindow`, menus, app dialogs, wiring services to widgets). No reusable controls or business logic.
- `pj_widgets/`: reusable Qt widgets usable by another Qt app, including `Timeline` (its `pj_runtime` binding `SourceTimelineController` lives in `pj_app`). Depends only on Qt and the standard library, never on other PJ modules.
- `pj_scene_common/`: backend-agnostic scene dock framework shared by 2D/3D; rendering-specific view state stays in the families.
- `pj_plotting/`, `pj_scene2D/`, `pj_scene3D/`: widget families with logic in `core/`, Qt widgets in `widget/` (`widgets/` for the scenes; for Scene3D the QRhi view, its passes and the layers), tests in `tests/`; `pj_scene2D/tools/` holds opt-in dev utilities (`PJ_BUILD_TOOLS`), `pj_scene3D/demos/` benchmarks (`PJ_BUILD_DEMOS`). No 3D rendering code outside `pj_scene3D`.
- `pj_custom_widgets/`: the Custom Widget panel family (series wiring to a Widget plugin's inputs, value-at-time push, persistence, plugin-object lifetime): widget-free logic in `core/`, the dock in `widget/`, tests in `tests/`. Plugin-created objects are destroyed synchronously, never with `deleteLater`.
- `pj_marketplace/`: extension registry, download, install/manage services and UI.
- `pj_dialog_host/`: host for plugin-provided dialogs. App dialogs stay in `pj_app`; reusable dialog controls in `pj_widgets`.
- `pj_scripting/`: Luau and embedded-Python engines behind a language-agnostic `ScriptEngine` seam. No scripting code in `pj_app` or widget modules unless strictly UI/editor code.
- `resources/`: shared resources in `resources.qrc`; module-local test/demo assets stay with their module. `resources/wasm-presets/` is the exception that is not compiled in: the browser sample presets `scripts/package_wasm.mjs --presets` deploys (see [`docs/WASM_PRESETS.md`](./docs/WASM_PRESETS.md)).
- `3rdparty/`: vendored source deps plus `retro/` (compliance artifacts installed next to `pj-raster-helper`). Conan/system deps do not belong here.
- `raster/`: `helper/` is the standalone GPL-2.0 `pj-raster-helper`; `ipc/` its header-only MPL-2.0 wire contract (also used by `pj_widgets`). PlotJuggler uses only the contract, never links the helper.
- `scripts/`: setup/maintenance utilities, `pre-commit/` hooks, `windows/` (PowerShell behind the `.bat` shims). The everyday `build.sh`, `run.sh`, `test.sh` and their shims stay at the root.
- `packaging/`: format-specific packaging: `nix/` (native build behind the root flake), `conda/` (source build for RoboStack users), repackaging a built tree (`appimage/`, `deb/`, `installer/`; [guides](./packaging/README.md)), and `web/` (the Worker that hosts the browser build at app.plotjuggler.io). Format utilities stay with their format; general dev utilities go to `scripts/`.

## Documentation

Each PJ4 module owns its intent docs. An agent landing in the repo reads root `CLAUDE.md` → per-module `CLAUDE.md` → per-module `docs/` → code. If any link in that chain is missing or stale, treat it as a documentation bug, not a code bug.

Record hard-won lessons in the owning module's `docs/TECHNICAL_NOTES.md`, written as the current state of the code (no history); prune them when the code changes.

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
- **Keep it lean:** a module CLAUDE.md holds what an agent needs before touching the code (purpose, invariants and pitfalls, one line per key header, tests, pointers). Judge each line by its value to that agent, not by a size limit; reference detail goes to the module's `docs/` or the header doc-comments.
- `<module>/docs/` — module-local intent docs. Minimum content depends on module complexity:
  - **Trivial / structurally obvious** (e.g. `pj_widgets`, `pj_dialog_host`): CLAUDE.md only; no `docs/` required.
  - **Standard module**: `docs/REQUIREMENTS.md` (the WHAT).
  - **Module with non-obvious internals**: add `docs/ARCHITECTURE.md` (the HOW). See `pj_scene2D/docs/` for the reference shape.

Cross-cutting docs (porting strategy, glossary, ADRs) live in top-level `docs/`. Its scope is described in [`docs/README.md`](./docs/README.md). Notably, [`docs/QT_NOTES.md`](./docs/QT_NOTES.md) records the current Qt baseline from [`versions.env`](./versions.env) and what changed since 6.8 — read it before using an unfamiliar Qt API.

### Module documentation index

| Module | Entry point | Docs |
|---|---|---|
| `pj_app` | [CLAUDE.md](./pj_app/CLAUDE.md) | [docs/](./pj_app/docs): ARCHITECTURE |
| `pj_runtime` | [CLAUDE.md](./pj_runtime/CLAUDE.md) | [docs/](./pj_runtime/docs): ARCHITECTURE, TECHNICAL_NOTES; derived objects |
| `pj_tf` | [CLAUDE.md](./pj_tf/CLAUDE.md) | — |
| `pj_widgets` | [CLAUDE.md](./pj_widgets/CLAUDE.md) | — |
| `pj_plotting` | [CLAUDE.md](./pj_plotting/CLAUDE.md) | [docs/](./pj_plotting/docs): ARCHITECTURE |
| `pj_custom_widgets` | [CLAUDE.md](./pj_custom_widgets/CLAUDE.md) | [docs/](./pj_custom_widgets/docs): REQUIREMENTS |
| `pj_dialog_host` | [CLAUDE.md](./pj_dialog_host/CLAUDE.md) | — |
| `pj_scene_common` | [CLAUDE.md](./pj_scene_common/CLAUDE.md) | — |
| `pj_scene2D` | [CLAUDE.md](./pj_scene2D/CLAUDE.md) | [docs/](./pj_scene2D/docs): REQUIREMENTS, ARCHITECTURE, TECHNICAL_NOTES |
| `pj_marketplace` | [CLAUDE.md](./pj_marketplace/CLAUDE.md) | [docs/](./pj_marketplace/docs): REQUIREMENTS, ARCHITECTURE, USER_MANUAL, marketplace-spec |
| `pj_scene3D` | [CLAUDE.md](./pj_scene3D/CLAUDE.md) | [docs/](./pj_scene3D/docs): REQUIREMENTS, ARCHITECTURE, TECHNICAL_NOTES, QRHI_NEXT (renderer follow-ups) |
| `pj_datastore` | [CLAUDE.md](./pj_datastore/CLAUDE.md) | [docs/](./pj_datastore/docs): REQUIREMENTS, ARCHITECTURE, USER_GUIDE, OBJECT_STORE_DESIGN; derived objects |
| `pj_scripting` | [CLAUDE.md](./pj_scripting/CLAUDE.md) | [docs/](./pj_scripting/docs): FILTER_CLASS; derived objects |
| Derived objects (cross-module: `pj_datastore`, `pj_runtime`, `pj_scripting`, `pj_scene*`, `pj_app`) | [README.md](./docs/derived_objects/README.md) | [ARCHITECTURE](./docs/derived_objects/ARCHITECTURE.md), [REQUIREMENTS](./docs/derived_objects/REQUIREMENTS.md) |
| `plotjuggler_sdk` (Conan package) | [GitHub repo](https://github.com/PlotJuggler/plotjuggler_sdk) | its own `CLAUDE.md` + `docs/` |

### Freshness discipline

Before any commit that changes behavior, public APIs, ABI structs, module ownership, or user-facing semantics: verify that the relevant `CLAUDE.md` / `docs/` files still match reality. If they don't, either update them in the same change or ask whether to update before committing. Do not commit known-stale docs.

## Key sources

**`plotjuggler_sdk`** (`pj_base` + `pj_plugins`, consumed as-is; its repo's `CLAUDE.md` tells the full story) is pinned as `plotjuggler_sdk/<version>` in `conanfile.txt`. To move to a new SDK: bump the pin there, then `conan lock add --requires=plotjuggler_sdk/<v>#<rrev>` on `conan.lock`. Sourcing details (JFrog remote, non-Conan fallback, `--sdk-local`): [docs/BUILDING.md](./docs/BUILDING.md#build-internals).

**The PJ3 reference tree (read-only)**: [github.com/facontidavide/PlotJuggler](https://github.com/facontidavide/PlotJuggler); point `PJ3_SOURCE_DIR` at a local checkout (no default). It is **the primary source for cherry-picked code**: `plotjuggler_app/` (plot widgets, docker, zoomers, `AxisTimeOffset`, tracker, drag-drop, per-curve transform UI, Lua engine), `plotjuggler_base/` (cross-check against `pj_base`), `plotjuggler_plugins/` (already ported; reference only). Treat it as read-only. Do not modify it from this repo. Plot widgets follow the "wholesale lift" (see `pj_plotting/CLAUDE.md`): port files largely intact, then rebind data reads (`PlotDataMapRef` → `DatastoreCurveAdapter` against `pj_datastore::DataReader`). Do not rebuild plot widgets from scratch.

## Build and test

C++20, CMake + Conan, Qt pinned by `PJ_QT_VERSION` in [`versions.env`](./versions.env) (read [docs/QT_NOTES.md](./docs/QT_NOTES.md) before using an unfamiliar Qt API). Guide, knobs and internals: [docs/BUILDING.md](./docs/BUILDING.md). Native x86_64 Linux Nix path (`nix build` / `nix run` / `nix develop`, no Conan or `.qt/`): [packaging/nix/README.md](./packaging/nix/README.md).

```bash
./scripts/install_qt6.sh             # one-time: pinned Qt into ./.qt/ (and git submodule update --init --recursive)
./build.sh                           # Conan + CMake + build into build/, incremental on re-run
./test.sh -R '^pj_runtime_tests\.'   # headless ctest; extra args go to ctest
./run.sh                             # launch through run.sh, not the bare binary
./build.sh --sanitize tsan           # lanes none|asan|tsan|msan, then ./test.sh --sanitize tsan (docs/SANITIZERS.md)
```

- **Windows:** `build.bat` / `run.bat` / `test.bat` take the same options; see [docs/BUILDING.md](./docs/BUILDING.md#windows-setup).
- **Worktrees:** `./scripts/worktree-new.sh <branch> [--base <ref>] [--build]` (base defaults to `origin/main`) and `./scripts/worktree-rm.sh <dir>` after the merge (deletes the branch too); never hand-roll `git worktree`: the scripts symlink the primary `.qt` by absolute path and borrow submodule objects offline.
- **Versioning rule: never hardcode a version.** `PJ_APP_VERSION`, `PJ_QT_VERSION`, `PJ_APPIMAGE_ARCH` live **only** in `versions.env`; read them from there and change them only there. Only the app version is overridable per build (`-DPJ_VERSION=<v>` / `PJ_VERSION`). The Qt platform dir tokens (`gcc_64`, `msvc2022_64`) are a separate namespace: leave them literal. Consumers: [docs/BUILDING.md](./docs/BUILDING.md#versions).
- **Confirm the recompile**: after a rebuild meant to pick up a C++ change, grep the build log for `Building .../<file>.cpp.o` (or check the `.o` mtime) before claiming the fix is live; RCC/QSS rebuilds can mask a stalled C++ recompile.
- **Never chain `pkill … ; ./build.sh`**: the chained build exits 144 and skips recompiling. Kill the running app in its own command, then run `./build.sh` standalone.
- **Portability:** Linux and Windows are shipped targets (macOS runs in CI). No Linux-only APIs or POSIX-specific paths in module code; gate platform code behind CMake / `#ifdef`. Pitfalls: [docs/BUILDING.md](./docs/BUILDING.md#portability).
- **Third-party:** vendored deps live in `3rdparty/` (never take them from Conan or the system); everything else comes from Conan. Qwt is fetched (pinned release), not vendored, and **never patched**: PJ-specific Qwt behavior lives in `pj_plotting` / `pj_dialog_host` ([details](./docs/BUILDING.md#third-party-dependencies)).

## UI conventions

- **Prefer `.ui` files over programmatic widget construction.** Widgets, layouts, menus, toolbars, dialogs — build them in Qt Designer (`.ui`) and load via `uic`. Use `AUTOUIC` in the module's `CMakeLists.txt`. Drop to hand-written `QWidget` subclasses only when the construction is genuinely dynamic (e.g. widgets created at runtime from plugin metadata) or when I explicitly ask for it.

## v1 scope

Parity-plus with PJ3: file + streaming sources, 11 built-in transforms, undo/redo, derived-series editor (incl. Lua and Python via `pj_scripting`), reactive scripts (via Toolbox + `onTimeChanged`), multi-tab workspace, marketplace install UI, all toolboxes, session recording of streaming sources to MCAP.

**Session recording is a runtime capability**, not a build flag: `RecordingService::isSupported()` decides and every Record affordance follows it (only the MCAP sink is desktop-only). One Record press = one `pj_<UTC timestamp>/` folder in the Preferences recording folder, holding ONE MCAP file per source (named after it, written straight into its final name), all sharing a `capture_id`; a file without a summary section was not stopped cleanly and still opens (`mcap recover` rebuilds its index); the write-queue budget is per source. See [docs/session_recorder_design.md](./docs/session_recorder_design.md).

Scene3D (wired into `pj_app` via `Scene3DDockWidget`), the plots and the Scene2D viewer render through QRhi (OpenGL, D3D11, Metal, WebGL2; `PJ_GRAPHICS_API` overrides at run time). Browser (WebAssembly) rules, details in [docs/WASM_DEPLOYMENT.md](./docs/WASM_DEPLOYMENT.md):

- Browser policy (file picking, bounded loading) stays on `PJ_TARGET_WASM`. Scene2D, Scene3D and on-demand Python are on by default; their Assimp, point-cloud-codec and CPython graphs enter only through `PJ_WASM_WITH_SCENE2D` / `_SCENE3D` / `_PYTHON`.
- Shader packs are committed `.qsb` files: re-bake them reproducibly with the pinned host `qsb`; the WASM configure hash-checks them.
- Every new static archive in the monolithic WASM executable needs a process-global-symbol collision audit (a clean link once hid a Qt/libjpeg collision), a `THIRD_PARTY_NOTICES.txt` entry (`cmake/PjWasmNotices.cmake`) and a source-offer entry (`scripts/wasm_source_offer.mjs`, automatic for `FetchContent_Declare` pins).
- The browser bundle is GPL-3.0 (it statically links Qt's GPL-3.0-only WebAssembly plugin); the desktop app stays MPL-2.0.
- Deterministic browser fixtures must reproduce bytes and the UI's product state: the Timeline prefers scalar/object bounds and extra TF samples do not widen it, so Scene3D seek fixtures need at least two object-bound samples.

## Non-goals (explicitly deferred)

- `StatePublisher` parity
- Exact 3.x UI/terminology parity
- Hot reload of running extension instances
- Full backward compatibility with 3.x layout files
- Recreating the removed prototype app as the final app

## Workflow notes

- Architectural questions → consult the placement rules above and the owning module's `CLAUDE.md` / `docs/`.
- New modules must respect the dependency rules above.
- Keep host code (`pj_app`, `pj_runtime`, `conanfile.txt`) **domain-neutral**: no plugin-specific terms (dataset-domain names, "episode") or hardcoded per-plugin policy. Mechanisms/protocols stay generic; the plugin supplies the domain meaning. A genuinely general capability motivated by one plugin is fine (e.g. a standard codec); a plugin-specific reference or branch in the host is not.
- Delegating to Codex: run it **harness-tracked with a watchdog** (`codex exec` in a backgrounded, timeout-bounded `Bash`); the companion job model sends no completion notification and Codex can hang silently. Judge liveness by job-log mtime, not the "running" status; on timeout, cancel and take over the build/test yourself. When it finishes, act on its findings (usually high quality).

### Execution autonomy

Once we've agreed on an approach, run the plan to completion — work through implementation, build, and tests **without pausing for "should I proceed?" check-ins**. The User reviews the final result, not each step; don't ask for mid-task confirmation on reversible work. "Run to completion" stops **at the commit boundary, not through it**: commits (see Commit policy), pushes/PRs, and anything outward-facing or destructive still require a stop-and-surface.

### Verifying visual / performance changes

Don't just assert a change works ("I don't see any difference" is a recurring round-trip); calibrate the claim to what you actually checked:

- **Visual / widget / layout changes** — a *suggestion*, not a rule: when cheap, take a screenshot (qt-widgeteer harness or `/run`); otherwise say honestly what you did and didn't confirm instead of implying it's visually verified.
- **Performance changes** — measurable, so give a **number**, not an assertion (replot/repaint rate, which should stay ≤60 Hz; frame time; a profiler counter).

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
- **Rebind data paths.** PJ3 wiring into `PlotDataMapRef` / `TransformsMap` becomes wiring into `pj_runtime` services (`CatalogModel`, `SessionManager`, `PlaybackEngine`, `DataProcessorService`). That's the one systematic rewrite.
- **Proactively surface improvement opportunities.** If you see a chance to improve separation of concerns, reusability, testability, or remove duplication while you're porting — flag it and **ask for approval before changing**. Don't silently refactor, and don't silently skip obvious wins. The bar is: "is there a cleaner shape that we'd regret not taking?" If yes, ask.
- **Don't fix what isn't broken.** Code that already reads cleanly and does the right thing gets ported close to verbatim (modulo style). Save the refactor energy for real problems.
