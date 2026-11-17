---
name: qa-session
description: Use when the user says "do QA", "test with the QA agent" or "try the app", when exploring PlotJuggler 4 like a user to find bugs unit tests miss (exploratory QA, a test charter, a repro of a UI bug), or when replaying a saved QA script. Covers the desktop app (an isolated offscreen instance driven through its MCP UI tools) and the browser build (Playwright plus the MCP data tools through a local relay), both with `scripts/qa/pjqa.py`.
---

# QA session

Drive a private, offscreen PlotJuggler as a user would: click, type, drag inside widgets, press keys, read the
widget tree, take screenshots. Find what the unit tests cannot: wiring between widgets, dialogs, focus, undo,
odd orders of actions. The design of the tools is in `docs/MCP_SERVER.md` "UI tools (QA)".

## Setup

```bash
PJ_BUILD_JOBS=8 ./build.sh --target pj_app          # the app the session runs (this checkout's build/)
export PJQA_HOME=<scratch dir>/pjqa                 # the instance's private HOME; one per instance
scripts/qa/pjqa.py start --plugin-dir ~/ws_plotjuggler/PJ-plugins -- \
  -d tests/wasm/fixtures/signals/data.csv -l tests/wasm/fixtures/signals/layout.pj4.xml
scripts/qa/pjqa.py call ui_tree                     # what is on screen, with refs
scripts/qa/pjqa.py call ui_click '{"ref":"w42"}'    # any MCP tool: name, then JSON arguments
scripts/qa/pjqa.py call screenshot                  # saves a PNG under $PJQA_HOME; open it with Read
scripts/qa/pjqa.py stop                             # always, at the end
```

- Linux only. Offscreen by default; `start --xvfb` runs under a virtual X display instead. Never run on the
  user's display: keys go to the active window.
- `--plugin-dir` names a folder of built plugins (CSV, MCAP, Parquet...); without it only built-in features load.
- Arguments after `--` go to the app: `-d <file>` loads data, `-l <layout>` restores plots on it. A file given
  without `-d` opens its loader's dialog, as a user's double-click would.
- `start` returns once the MCP server answers; a load or dialog may still be on its way. Call `ui_tree` until
  the screen settles.
- A fresh `PJQA_HOME` is a first run (no settings). Reusing one keeps the settings an earlier session changed.

## The loop

1. `ui_tree`, read it: one line per widget, `[ref] Class#objectName "text" (flags) WxH`. Flags include
   `disabled`, `checked`, `read-only`, `focus`, `modal`. The modal dialog and open popup come first; windows
   behind it are one line and cannot be acted on.
2. Act: `ui_click` (`double`, `button`, `x`/`y`, `modifiers`), `ui_type`, `ui_key` (`"Ctrl+Z"`, `"Escape"`),
   `ui_drag` (`from`/`to` inside one widget), `ui_wheel`. Target by `ref`, or by `name` (an objectName) when
   exactly one visible widget has it.
3. Check the effect: `ui_tree` again, `get_status` (datasets, time, playing), `screenshot` for anything visual.
   The data tools (`list_topics`, `read_series`...) confirm what was loaded.
4. Refs belong to the latest `ui_tree`. Anything that rebuilds widgets (undo, loading a layout, closing a tab)
   makes them stale: list again.

Pitfalls seen in practice:

- **Drag-and-drop between widgets does not work** (curve list onto a plot, dock moves): Qt's drag loop reads the
  real pointer, which these tools do not move. Gestures inside one widget work: zoom box, pan, splitters,
  sliders. Build plots from a layout (`-l`) or through menus instead.
- `ui_type` appends to what a field holds: `ui_key` `"Ctrl+A"` on it first to replace.
- A `read-only` field ignores typing by design (e.g. `displayTime`, the playback readout).
- Some settings persist instead of entering undo (the toolbar's grid toggle): not every change is undoable.
- Menus: click the menu bar item, then `ui_tree` lists the popup's actions with refs. `screenshot` does not show
  popups or non-modal windows (Diagnostics, a floating toolbox): read them from `ui_tree`. Items made of widgets
  (the Datasets options menu) list as `item ""`: click by ref, and the reply's "landed on" names them.
- A plot area's split and fullscreen buttons show only on hover: `ui_click` the toolbar's `widgetButtons` once,
  then `buttonSplitHorizontal`, `buttonSplitVertical` and `buttonFullscreen` appear in `ui_tree`.
- Wheel zoom and pan reach the undo stack after a short debounce: give such a call `"wait_ms":400` before pressing
  undo, or the undo arrives first.
- `ui_tree` lists dock areas in creation order, not screen order: check which plot is which by its title or a
  screenshot. A name that every plot repeats (`qwtCanvas`, `buttonClose`) is refused while two plots show; scripts
  that need one use a single-plot layout.
- Plots fall back to the raster canvas offscreen (no GPU): the GPU plot path is not exercised.

## Browser build

The browser (WebAssembly) build has the data tools but not the UI tools. Click, type and look with
Playwright; check results with the same `pjqa.py call`, through a local stand-in for the agent relay.

```bash
# Package once. The pinned toolchains live in the primary checkout (.emsdk, .qt-wasm/<PJ_QT_VERSION>),
# also from a worktree. Browser video needs meson on PATH (`uv tool install meson`).
MAIN=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)"); source versions.env
source "$MAIN/.emsdk/emsdk_env.sh"
QT=$MAIN/.qt-wasm/$PJ_QT_VERSION
cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release -DPJ_BUILD_TESTS=OFF -DPJ_BUILD_DEMOS=OFF \
  -DCMAKE_TOOLCHAIN_FILE=$QT/wasm_multithread/lib/cmake/Qt6/qt.toolchain.cmake -DQT_HOST_PATH=$QT/gcc_64 \
  -DPJ_WASM_PYTHON_CACHE_DIR=$HOME/.cache/pj-wasm-cpython
host-job wasm-build bash -c 'cmake --build build-wasm --target pj_app --parallel 8 && node scripts/package_wasm.mjs \
  --source build-wasm/pj_app --output build-wasm/deploy --presets resources/wasm-presets'
until [ -e ~/.cache/pj-data-platform/jobs/wasm-build.exit ]; do sleep 20; done   # host-job runs detached

scripts/qa/pjqa.py start --browser build-wasm/deploy   # serves it, starts the relay, prints the URL
```

- Open the printed URL with the Playwright tools (`browser_navigate`). The app takes 10-20 s to boot; it then
  answers `pjqa.py call get_status`.
- `pjqa.py call open_file '{"path":"/abs/file.csv"}'` loads a local file: the relay serves it to the page. Then
  `list_topics`, `read_series`, `screenshot`, `playback`... as on the desktop. `ui_*` tools answer
  "unknown tool": that is the browser build, not a bug.
- Input: the whole app is one canvas, so `browser_snapshot` shows nothing useful. Read positions from
  `browser_take_screenshot`, then click with `browser_run_code_unsafe`:
  `async (page) => { await page.mouse.click(715, 533); await page.waitForTimeout(500); }`. The same function
  does `page.mouse.wheel`, `page.mouse.down/move/up` drags and `page.keyboard.press('Control+Z')`.
  Keys reach the app only while Qt's hidden input holds focus. If a key does nothing, check
  `document.activeElement` before reporting it: on Qt 6.12 a press the clicked widget does not accept moves
  focus to `<body>` (QTBUG-144692).
- `browser_take_screenshot` refuses an absolute `filename` outside the session folder: pass a relative name.
  Inside `browser_run_code_unsafe`, `page.screenshot({path, clip})` saves a crop anywhere under `PJQA_HOME`.
- File > Load Layout opens the browser's file chooser: click the item, then `browser_file_upload` with the
  absolute path. Save Layout downloads: `page.waitForEvent('download')`, then `saveAs`.
- After anything visible, `browser_console_messages` (level `error`) catches aborts and failed fetches.
- A page opened with `?mcp=` (as `pjqa.py` prints it) skips the welcome dialog. The sample presets are under
  File > Presets: each Open loads a dataset and a layout in one click, the quickest way to plots.
- One browser session at a time: every agent in a Claude session shares the one Playwright browser.
- `pjqa.py stop` ends the server and the relay; `browser_close` closes the page.

## What to report

For each finding: what you did, what you expected, what happened, how sure you are. Attach the steps as a
replay script (below) and the screenshot path. Separate:

- **Bug**: wrong result, crash (`start`'s `app.log` under `$PJQA_HOME`, or the app vanishes), lost data, a
  control that does nothing it claims.
- **UX**: works but confuses (no feedback, unclear text, an unlabeled control in `ui_tree`).
- **Tool limit**: the tools could not do it (say which); not a PlotJuggler bug.

Before calling something a bug, reproduce it once more from a fresh `PJQA_HOME`.

## Replay scripts

`scripts/qa/pjqa.py run <file>` replays one step per line and stops at the first failure:

```
# a tool call; it must succeed
ui_click {"name":"buttonPlay"}
# this call must fail
refuse ui_click {"name":"noSuchWidget"}
get_status {}
# the previous result contains this text
expect "playing":true
```

A `#` starts a comment only at the beginning of a line.

Name widgets by objectName in scripts, never by ref (refs change between runs). `scripts/qa/smoke.pjqa` is the
example, over the signals fixture.

## Charters

Pick one per session, stay within it, stop after about 30 minutes or 150 tool calls:

1. **Load and look**: load the signals CSV through the Load button and its dialog; expand the tree; change the
   CSV dialog's options (time axis, delimiter) and reload.
2. **Plots**: with the signals layout, zoom (box, wheel), pan, undo/redo each, link and unlink X, close and
   reopen a plot area, add a tab, rename it, close it, undo.
3. **Playback**: play, pause (button and Space), loop, speed and step scrubbers, slider seeks; time offset and
   clock time buttons.
4. **Dialogs**: every menu entry; Escape and Cancel on each dialog; Preferences: change, cancel, reopen.
5. **Layouts**: save the layout, start fresh, load it back; compare with screenshots.
6. **Browser**: the sample presets under File > Presets (each Open), a CSV through `open_file`, plots, zoom
   and undo, playback, a layout reload; console errors after each step.

## A full QA pass ("do QA")

When asked to do QA or to test with the QA agent, without a narrower charter:

1. Build the checkout under test: `./build.sh` (desktop) and, for the browser, the package above. Use a
   plugin folder for the desktop (`--plugin-dir`, e.g. pj-official-plugins' `build/all/Release/bin`).
2. Start `qa-engineer` agents in one message: one per desktop charter (1-5), each with its own `PJQA_HOME`
   under the scratchpad, plus one with the browser charter (6). Give each the checkout path and, for the
   browser one, the package path.
3. Check every reported **Bug** yourself against the code and its replay before acting on it; drop artifacts.
4. Report a table: charter, bugs, UX notes, tool limits. Fix each confirmed bug in its own PR, with a test that
   fails before the fix.
