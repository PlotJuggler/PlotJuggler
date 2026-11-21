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
- `ui_tree` lists dock areas and tabs in creation order, not screen order: check which plot is which by its title,
  and the order of the tabs on screen, with a screenshot. A name that every plot repeats (`qwtCanvas`, `buttonClose`) is refused while two plots show; scripts
  that need one use a single-plot layout.
- Under `--xvfb` the app counts as attended: `-d a -d b -l <layout>` with both files carrying the layout's topics
  opens the which-datasets question at startup; answer it with the UI tools. Offscreen it takes the first dataset.
- Plots fall back to the raster canvas offscreen (no GPU): the GPU plot path is not exercised.
- No tool reports marker rules or the marker regions on a plot: a bug there shows only in a screenshot, so it
  cannot get a regression script. Say so in the fix PR.

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
# ... and not this one
absent "playing":false
# repeat a call until its result holds this text (at most 30 s): a load, a layout, a debounced undo
until ui_plot {"title":"Periodic"} "time : 400.000"
# repeat a call until the number at this top-level key of its JSON result exceeds the first call's (at most 30 s)
grows get_status {} current_time
```

A `#` starts a comment only at the beginning of a line.

Name widgets by objectName in scripts, never by ref (refs change between runs). Assert on what the user reads:
`ui_tree` for widgets, `ui_plot` for a plot's ranges, curves and tracker value box, the data tools for data. A
screenshot is evidence for a human, not an assertion.

`ui_tree` clips a row to 80 characters, and a list shows only the rows in its viewport: filter before asserting
`absent` on long or scrolled text. `expect` / `absent` match the text literally, quotes included: `until ui_tree {}
"Scene3DDockWidget"` never matches `PJ::Scene3DDockWidget`. Check an `absent` once against a state that holds the
text, or it may pass for the wrong reason.

## Regression scripts

`scripts/qa/regressions/*.pjqa` is the suite. Each script starts with its instance's arguments, paths relative to
the checkout, and is replayed in a fresh instance:

```
# app: -d tests/wasm/fixtures/signals/data.csv -l tests/wasm/fixtures/signals/layout.pj4.xml
# needs: plugins
```

```bash
scripts/qa/pjqa.py regress --plugin-dir <plugins>       # all of them: a PASS/FAIL/SKIP table, exit 1 on a FAIL
scripts/qa/pjqa.py regress scripts/qa/regressions/smoke.pjqa
```

- Without `--plugin-dir`, scripts marked `# needs: plugins` are skipped. The CSV loader of a release is enough for
  the signals fixture: `gh release download data_load_csv/v<version> -R PlotJuggler/pj-official-plugins` and unzip.
- A failed script keeps its instance's home (`app.log`, screenshots); the table prints its path.
- `pjqa.py start` returns before a `-d` / `-l` load finishes: open with an `until` on the state the script needs.
  Use `until`, not `expect`, after anything the app finishes later (a seek reaches the tracker within ~33 ms, an
  undo after its debounce).
- Small fixtures of its own go in `scripts/qa/regressions/fixtures/`.
- Run the suite before handing back a change that touches what a script covers, and after each QA session.

## Charters

Pick one per session, stay within it, stop after about 30 minutes or 150 tool calls:

1. **Load and look**: load the signals CSV through the Load button and its dialog; expand the tree; change the
   CSV dialog's options (time axis, delimiter) and reload.
2. **Plots**: with the signals layout, zoom (box, wheel), pan, undo/redo each, link and unlink X, close and
   reopen a plot area, add a tab, rename it, drag it to another position, close it, undo.
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
   browser one, the package path. Pass `model:` explicitly:
   - **A release pass** runs every charter twice, once on `opus` and once on `sonnet`. Over ten charters run on
     both, the two models found mostly different bugs (opus 22, sonnet 16, 7 in common, 31 in all), and either
     one alone would have missed the worst bug the other found. Sonnet's cost per charter was about the same as
     opus', because it used more tokens and more tool calls.
   - **A quick pass** runs `opus` only.
   - Keep every app under `host-job --light` and start the instances a few seconds apart.
3. Check every reported **Bug** yourself against the code and its replay before acting on it; drop artifacts.
   Check it against current `origin/main` too (`git log origin/main -- <area>`): a pass runs on a fixed build,
   and a bug may already be fixed on main.
4. Report a table: charter, bugs, UX notes, tool limits.
5. Fix each confirmed bug as below.

## Fixing what a pass found

- **One PR per bug, test first:** a unit or GUI test that fails before the fix, and a script in
  `scripts/qa/regressions/` that asserts the user-visible result (`expect` / `absent` on `ui_plot`, `ui_tree` or a
  data tool) and fails on the build before the fix. When no tool can see the state, the PR says so.
- **Screenshots in the PR** whenever the bug or the fix shows on screen (skip it, and say so, for a tool reply or a
  data value): "before" is the QA agent's screenshot of the bug, "after" a `screenshot` on the fixed build replaying
  the same steps. Point at the problem in each one with `scripts/qa/shots.py annotate`: a rectangle around it, an
  arrow to it, a short label ("time column picked: b", "markers gone after undo"); crop to the area that matters.
  Read the PNG first to get pixel coordinates, and read the result to check that nothing important is covered.
  `scripts/qa/shots.py publish <fix-branch> "Before: ...=before.png" "After: ...=after.png"` puts them on the
  `qa-screenshots` branch and prints the Markdown for the PR body (`gh` cannot upload images).
- **Fix the root cause in the shared mechanism**, not where the symptom showed, and say in the PR which other paths
  the fix covers. Skip contrived inputs: they get a line in the report, not a PR.
- **Host or plugin:** when either could hold the fix, count who each one reaches. A plugin fix reaches every
  PlotJuggler 4 user once the plugin is released; a host or SDK fix reaches only new app versions, and an SDK change
  also needs an SDK release first.
- **A plugin persists a choice by name or by the file it was made for, never by position.** A column index or a
  selection saved for one file and pre-filled for the next picked the wrong data in three loaders at once.
- **A plugin fix adds its CHANGELOG bullets under `## Upcoming`** and leaves the plugin's version alone; the version
  changes only when the user asks for that plugin's release (pj-official-plugins' `CLAUDE.md`, Release Process). List
  the plugins with unreleased Upcoming entries in the pass report, so the user can decide what to release.
- **Every fix PR gets an independent reviewer** (a fresh agent with the PR's goal and diff, `model: opus`), however
  small the fix. In one pass the reviewer found a real defect in the first version of four fixes. Apply the fixes it
  finds that matter, and add the review and anything not applied to the PR body.
- **PR bodies:** write the body to a file with a quoted heredoc (`<<'EOF'`), then pass it with `--body-file`, or
  with `gh api -X PATCH .../pulls/N -F body=@file` for an update. Backticks inside an unquoted heredoc or a
  `-b` string run as shell commands.
- **Merge one PR at a time.** After each merge, rebase the next PR onto the new `main` and wait for its CI, even if
  it was green before. Two PRs that were each green broke `main`'s build together: one changed an API that the
  other's new test called.
