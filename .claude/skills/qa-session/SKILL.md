---
name: qa-session
description: Use when exploring PlotJuggler 4 like a user to find bugs unit tests miss (exploratory QA, a test charter, a repro of a UI bug), or when replaying a saved QA script. Drives an isolated, offscreen app through its MCP UI tools (`--mcp-ui-tools`) with `scripts/qa/pjqa.py`.
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
  popups: read them from `ui_tree`.
- Wheel zoom and pan reach the undo stack after a short debounce: give such a call `"wait_ms":400` before pressing
  undo, or the undo arrives first.
- `ui_tree` lists dock areas in creation order, not screen order: check which plot is which by its title or a
  screenshot. A name that every plot repeats (`qwtCanvas`, `buttonClose`) is refused while two plots show; scripts
  that need one use a single-plot layout.
- Plots fall back to the raster canvas offscreen (no GPU): the GPU plot path is not exercised.

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
