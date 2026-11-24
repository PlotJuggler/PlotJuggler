# PlotJuggler 4 (PJ4)

A desktop and browser application for time-series and robotics data, built on `plotjuggler_sdk`. Work here is
maintenance, fixes and new features. PlotJuggler 3 lives on the `main-3.x` branch; it is a different codebase,
not a model for PJ4 code.

## License

PJ4 is **MPL-2.0** ([`LICENSE`](./LICENSE)): every source file starts with `// SPDX-License-Identifier: MPL-2.0`,
and new files must too. Files under other licenses keep their own headers (`pj_runtime/agent_tools/` is MIT,
`raster/helper/` GPL-2.0). The browser bundle is GPL-3.0, because it statically links Qt's GPL-3.0-only
WebAssembly plugin; the desktop app stays MPL-2.0. `plotjuggler_sdk` is a separate repo with its own license
(Apache-2.0 for `pj_base`/`pj_plugins`); do not relicense it from here.

## Architecture

Each module's `CLAUDE.md` and `docs/` describe its implementation. Cross-module designs: derived series and objects
in [docs/derived_objects/](./docs/derived_objects/ARCHITECTURE.md), recording and the source cache in
[RECORDING_AND_CACHING.md](./docs/RECORDING_AND_CACHING.md), AI agents over MCP in
[MCP_SERVER.md](./docs/MCP_SERVER.md); the rest of `docs/` is indexed in [docs/README.md](./docs/README.md).

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
├── pj_log/            # Log panel: log topics merged by time in a filterable list
├── pj_app/            # main window shell
├── resources/         # SVG icons + resources.qrc; wasm-presets/ browser sample index
├── raster/            # helper/ executable + ipc/ wire contract
├── scripts/           # setup, worktrees, pre-commit, windows/ entry points
├── packaging/         # Nix, conda, AppImage, Debian package, Windows installer, macOS dmg, hosted browser app
└── docs/              # cross-cutting designs and guides
```

The widget families (`pj_plotting`, `pj_custom_widgets`, `pj_log`, `pj_scene2D/widgets` via the `pj_scene2d_widgets`
target, `pj_scene3D/widgets` via `pj_scene3d_widgets`) never depend on each other. Shared Qt controls live in
`pj_widgets`; shared runtime state flows through the `IDataWidget` contract in `pj_runtime`. Plots, Scene2D and Scene3D
render through QRhi (OpenGL, D3D11, Metal, WebGL2; `PJ_GRAPHICS_API` overrides the backend at run time).

### Placement rules

Add files to the owning module, not to new top-level folders. If a requested location does not fit these
boundaries, ask before proceeding and suggest the closest fit.

- `plotjuggler_sdk` (external): the plugin SDK (`pj_base`, `pj_plugins`; canonical object schemas and codecs under
  `pj_base/builtin/`), linked as `plotjuggler_sdk::base` / `::plugin_sdk` / `::plugin_host`. Change it in the
  [SDK repo](https://github.com/PlotJuggler/plotjuggler_sdk), then release and bump the pin here.
- `pj_datastore/`: columnar `DataEngine`, `ObjectStore`, `IncrementalEngine`, host-side C-ABI write bridges and the
  Qt-free, Luau-free `PJ::proc::DataProcessor` substrate (`pj_scripting` layers the engines on top). Pure C++20,
  depends only on `pj_base`; app-internal (plugins reach it through the C ABI).
- `pj_tf/`: TF frame tree, a static Qt-free leaf (only `glm`, `plotjuggler_sdk::base`, `nlohmann_json`). Ownership,
  ingest and signals stay in `pj_runtime`; rendering in `pj_scene3D`.
- `pj_runtime/`: runtime services and contracts (session, catalog, playback, extensions, TF, derived engines). No
  concrete widgets, no `Qt6::Widgets` link.
- `pj_app/`: executable shell only (`MainWindow`, menus, app dialogs, wiring services to widgets). No reusable
  controls or business logic.
- `pj_widgets/`: reusable Qt widgets usable by another Qt app, including `Timeline` (its `pj_runtime` binding
  `SourceTimelineController` lives in `pj_app`). Depends only on Qt and the standard library.
- `pj_scene_common/`: backend-agnostic scene dock framework shared by 2D/3D; rendering-specific view state stays in
  the families.
- `pj_plotting/`, `pj_scene2D/`, `pj_scene3D/`: widget families with logic in `core/`, Qt widgets in `widget/`
  (`widgets/` for the scenes), tests in `tests/`; `pj_scene2D/tools/` holds opt-in dev utilities
  (`PJ_BUILD_TOOLS`), `pj_scene3D/demos/` benchmarks (`PJ_BUILD_DEMOS`). 3D rendering code lives only in
  `pj_scene3D`.
- `pj_custom_widgets/`: the Custom Widget panel family: widget-free logic in `core/`, the dock in `widget/`, tests in
  `tests/`. Plugin-created objects are destroyed synchronously: a deferred delete would let the session or the
  event loop reach them after their plugin handle is released.
- `pj_log/`: the Log panel family: widget-free logic (the row store and filter, Qt-free) in `core/`, the view and the
  dock in `widget/`, tests in `tests/`.
- `pj_marketplace/`: extension registry, download, install/manage services and UI.
- `pj_dialog_host/`: host for plugin-provided dialogs. App dialogs stay in `pj_app`; reusable dialog controls in
  `pj_widgets`.
- `pj_scripting/`: Luau and embedded-Python engines behind a language-agnostic `ScriptEngine` seam. Scripting code
  stays out of `pj_app` and the widget modules, except UI/editor code.
- `resources/`: shared resources in `resources.qrc`, desktop-only splash memes in `memes.qrc`; module-local test and
  demo assets stay with their module. `resources/wasm-presets/` is not compiled in: it holds the browser sample
  presets' index that `scripts/package_wasm.mjs --presets` deploys and app.plotjuggler.io serves from an upload
  ([packaging/web](./packaging/web/README.md#sample-index)); their layouts, thumbnails and data live on the asset
  host ([docs/WASM_PRESETS.md](./docs/WASM_PRESETS.md)).
- `3rdparty/`: vendored source deps plus `retro/` (compliance artifacts installed next to `pj-raster-helper`).
  Conan and system deps do not belong here.
- `raster/`: `helper/` is the standalone GPL-2.0 `pj-raster-helper`; `ipc/` its header-only MPL-2.0 wire contract
  (also used by `pj_widgets`). PlotJuggler uses only the contract: linking the helper would bring its GPL-2.0
  license into the app.
- `scripts/`: setup and maintenance utilities, `pre-commit/` hooks, `windows/` (PowerShell behind the `.bat` shims).
  The everyday `build.sh`, `run.sh`, `test.sh` and their shims stay at the root.
- `packaging/`: one folder per format: `nix/` (native build behind the root flake), `conda/` (source build for
  RoboStack users), `appimage/`, `deb/`, `installer/`, `macos/` (repackaging a built tree;
  [guides](./packaging/README.md)), `web/` (the Worker that hosts the browser build at app.plotjuggler.io).
  Format utilities stay with their format; general dev utilities go to `scripts/`.

## Documentation

An agent reads root `CLAUDE.md` → the module's `CLAUDE.md` → its `docs/` → the code. A missing or wrong link in that
chain is a documentation bug.

**Docs and comments say what is true now.** No history, no change notes, no plans, no PR, phase or decision
numbers; git and the PRs keep the past. Before committing a change to behaviour, public APIs, ABI structs, module
ownership or user-facing semantics, check that the affected `CLAUDE.md`, docs and comments still match; update
them in the same change, or ask before committing. Never commit known-stale docs.

**Past decisions are evidence, not rules.** Where a doc explains why something is built the way it is, it says what
was observed or tried, under which conditions, and when it would be worth trying again; reasoning that was never
tested is labelled as reasoning. With new data or a better idea, challenge an earlier choice: say so and show the
data. (The commit policy below still applies.)

Per module (`pj_*/`):

- `CLAUDE.md`, always: purpose, the invariants a change would break (each with what breaks), one line per key
  header, how to run its tests, pointers to its docs.
- `docs/ARCHITECTURE.md`, when the internals are not obvious: how it works today (components, data and control
  flow, threading and lifetime, cross-module and behaviour contracts, recipes for common changes) and its known
  gaps, one line each.
- `docs/TECHNICAL_NOTES.md`, when there is evidence: one entry per lesson with what was observed or tried, the
  conditions (versions, platform, sizes), what the code does about it, and when to re-test.

Cross-module designs live in top-level `docs/` with the same split; a single-file design ends with an Evidence
section. The code holds what drifts (signatures, member names, constant values, file lists, test names): docs name
the class or header and describe the behaviour, quote a number only when it is a contract (and name the symbol that
defines it), and refer to other doc sections by heading, never by number.

**Comments say only what the code cannot:**

- invariants a change would break: lock order, destruction order, thread affinity, ownership and lifetime, call
  order, units, time bases, sentinel values;
- external contracts: C ABI, file and wire formats, persisted names;
- a non-obvious reason, in a clause; a workaround names the external component and version, so it can be re-tested.

No comment on a self-explanatory declaration, no narration of the code. "must" and "never" come with what breaks.
[docs/QT_NOTES.md](./docs/QT_NOTES.md) records the Qt baseline from [`versions.env`](./versions.env) and what
changed since 6.8; read it before using an unfamiliar Qt API.

## Build and test

C++20, CMake + Conan, Qt pinned by `PJ_QT_VERSION` in [`versions.env`](./versions.env). Guide, knobs and internals:
[docs/BUILDING.md](./docs/BUILDING.md). Native x86_64 Linux Nix path (`nix build` / `nix run` / `nix develop`, no
Conan or `.qt/`): [packaging/nix/README.md](./packaging/nix/README.md).

```bash
./scripts/install_qt6.sh             # one-time: pinned Qt into ./.qt/ (and git submodule update --init --recursive)
./build.sh                           # Conan + CMake + build into build/, incremental on re-run
./test.sh -R '^pj_runtime_tests\.'   # headless ctest; extra args go to ctest
./run.sh                             # launch through run.sh, not the bare binary
./build.sh --sanitize tsan           # lanes none|asan|tsan|msan, then ./test.sh --sanitize tsan (docs/SANITIZERS.md)
```

- **Windows:** `build.bat` / `run.bat` / `test.bat` take the same options
  ([setup](./docs/BUILDING.md#windows-setup)).
- **Worktrees:** `./scripts/worktree-new.sh <branch> [--base <ref>] [--build]` (base defaults to `origin/main`), and
  `./scripts/worktree-rm.sh <dir>` after the merge (deletes the branch too). Do not hand-roll `git worktree`: the
  scripts symlink the primary `.qt` by absolute path and borrow submodule objects offline.
- **SDK pin:** `plotjuggler_sdk/<version>` in `conanfile.txt`; to move, bump it there, then
  `conan lock add --requires=plotjuggler_sdk/<v>#<rrev>` on `conan.lock`. Sourcing (JFrog remote, non-Conan
  fallback, `--sdk-local`): [docs/BUILDING.md](./docs/BUILDING.md#build-internals).
- **Versions are never hardcoded.** `PJ_APP_VERSION`, `PJ_QT_VERSION` and `PJ_APPIMAGE_ARCH` live only in
  `versions.env`: read them from there, change them only there. Only the app version is overridable per build
  (`-DPJ_VERSION=<v>` / `PJ_VERSION`). Qt platform dir tokens (`gcc_64`, `msvc2022_64`) are a separate namespace:
  leave them literal. Consumers: [docs/BUILDING.md](./docs/BUILDING.md#versions).
- **Confirm the recompile:** after a rebuild meant to pick up a C++ change, grep the build log for
  `Building .../<file>.cpp.o` (or check the `.o` mtime) before claiming the fix is live; RCC/QSS rebuilds can mask a
  stalled C++ recompile.
- **Do not chain `pkill … ; ./build.sh`:** the chained build exits 144 and skips recompiling. Kill the app in its own
  command, then run `./build.sh` alone.
- **Portability:** Linux, Windows and macOS (arm64) are shipped targets. No Linux-only APIs or POSIX-specific paths
  in module code; gate platform code behind CMake / `#ifdef` ([pitfalls](./docs/BUILDING.md#portability)).
- **Third-party:** vendored deps live in `3rdparty/`, never taken from Conan or the system (only distribution builds
  opt out, with `PJ_SYSTEM_*`); everything else comes from Conan. Every package ships `THIRD_PARTY_NOTICES.txt`
  (`cmake/PjNotices.cmake`, [Licences](./packaging/README.md#licences)): a new dependency needs its licence there.
  Qwt is fetched as a pinned release and not patched: PJ-specific Qwt behavior lives in `pj_plotting` /
  `pj_dialog_host` ([details](./docs/BUILDING.md#third-party-dependencies)).
- **Browser build** ([docs/WASM_DEPLOYMENT.md](./docs/WASM_DEPLOYMENT.md)): browser policy (file picking, bounded
  loading) stays on `PJ_TARGET_WASM`; the Assimp, point-cloud-codec and CPython graphs enter only through
  `PJ_WASM_WITH_SCENE2D` / `_SCENE3D` / `_PYTHON`. Shader packs are committed `.qsb` files, re-baked with the
  pinned host `qsb` and byte-compared at configure time. Every new static archive in the monolithic WASM executable
  needs a process-global-symbol collision audit (a clean link once hid a Qt/libjpeg collision), a
  `THIRD_PARTY_NOTICES.txt` entry (`cmake/PjWasmNotices.cmake`) and a source-offer entry
  (`scripts/wasm_source_offer.mjs`, automatic for `FetchContent_Declare` pins).

## UI conventions

- **Prefer `.ui` files over programmatic widget construction.** Widgets, layouts, menus, toolbars, dialogs: build
  them in Qt Designer and load them via `uic` (`AUTOUIC` in the module's `CMakeLists.txt`). Hand-written `QWidget`
  construction only when it is genuinely dynamic (widgets created at run time from plugin metadata) or when I ask.
- **`.ui` `objectName`s are stable.** Layout files, stylesheet selectors and tests depend on them (e.g.
  `buttonLoadDatafile`, `displayTime`, `playbackLoop`, `streamingSpinBox`). Rename one only when I ask.

## Workflow

- Host code (`pj_app`, `pj_runtime`, `conanfile.txt`) stays **domain-neutral**: no plugin-specific terms
  (dataset-domain names, "episode") or hardcoded per-plugin policy. Mechanisms and protocols stay generic; the
  plugin supplies the meaning. A general capability motivated by one plugin is fine (e.g. a standard codec).
- "Do QA", "test with the QA agent", exploratory QA through the UI (a charter, or a UI bug to reproduce): load the
  `qa-session` skill and follow it. Its `qa-engineer` agents drive an isolated offscreen desktop app and the browser
  build with `scripts/qa/pjqa.py`, never on the user's display. A request without a narrower charter means the
  skill's "full QA pass": every charter, desktop and browser.
- Delegating to Codex: run it **harness-tracked with a watchdog** (`codex exec` in a backgrounded, timeout-bounded
  `Bash`); the companion job model sends no completion notification and Codex can hang silently. Judge liveness by
  the job log's mtime, not the "running" status; on timeout, cancel and take over the build/test yourself. Check
  its findings against the code before acting on them.

### Execution autonomy

Once we've agreed on an approach, run the plan to completion (implementation, build, tests) **without pausing for
"should I proceed?" check-ins**. I review the final result, not each step; don't ask for mid-task confirmation on
reversible work. "Run to completion" stops **at the commit boundary, not through it**: the actions listed under
Commit policy need my go-ahead, and nothing else does.

### Verifying visual and performance changes

Calibrate the claim to what you actually checked:

- **Visual / widget / layout changes:** when cheap, take a screenshot (the MCP `screenshot` tool,
  `plotjuggler4 --screenshot <png>` for the first 3D view, or `/run`); otherwise say what you did and did not
  confirm instead of implying it is visually verified.
- **Performance changes:** give a **number**, not an assertion (replot/repaint rate, which should stay ≤60 Hz;
  frame time; a profiler counter).

### Commit policy

- **Get my explicit go-ahead before each of these actions**, by surfacing the diff (or the exact command) and
  asking:
  - force-pushing a branch someone else owns;
  - creating, merging, closing or commenting on a PR or an issue;
  - tags and releases, Conan and plugin-registry uploads, production deploys (`app.plotjuggler.io`);
  - `./scripts/worktree-rm.sh` and deleting a branch.
- **A PR merges when every check is green.** `main` has no required checks, and merges made seconds after opening
  broke it. `.claude/hooks/pr-merge-check.py` warns (never refuses) when `gh pr merge` runs while a check fails or
  runs, or before CI has registered.
- **A go-ahead names the action and comes after I have seen the diff.** One given for an earlier task does not
  carry over to the next.
- **Exception, documentation only:** a change that touches nothing but documentation (`docs/`, module `docs/`,
  `CLAUDE.md` files, `.claude/skills/` and `.claude/agents/`) is committed and pushed straight to `main` right away,
  with no PR, once `pre-commit` passes on it.
- **No go-ahead needed:** `git commit`, `git push` to your own branch (force-push included; `main` only for the
  documentation exception above), `git fetch`, new worktrees and branches, builds, `./test.sh`, read-only `git` and
  `gh` commands, re-running or dispatching CI for an existing branch, deleting build and scratch directories.

### Git gotchas

- After `git mv` plus edits to the moved file, `git add` the new path: the edits are not staged with the rename
  ("100% similarity" in the commit output is the tell).
- `git rebase` skips pre-commit hooks. After a rebase with **manual conflict resolution**, run
  `pre-commit run --all-files` and fold fixes into the originating commit (`--fixup` + `--autosquash`).
- After a squash-merge, verify every intended commit landed (`git log --oneline <merge-commit>`): commits pushed
  after the squash diff was prepared are silently dropped.
- After merging `origin/main` into a feature branch, check that the feature's entry points still have **production
  callers** (not just definitions and tests): a modify/delete conflict can drop wiring while CI stays green.

### Reuse before writing

- **Search before you add.** Before writing a widget, helper or service, look for one that exists: the owning
  module's `CLAUDE.md` key-header list first (reusable Qt controls are listed in `pj_widgets/CLAUDE.md`), then the
  code. Reuse or extend it; if nothing fits, say in the summary where you looked.
- **Match the module you are in:** its file naming, namespace and member conventions. clang-format owns the layout
  (Google C++, 2-space, 120 columns).
- **Surface improvement opportunities.** When a cleaner shape near your change would improve separation of concerns,
  reusability or testability, or remove duplication, flag it and **ask before changing**. Code that already reads
  cleanly and does the right thing stays as it is.
