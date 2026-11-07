# Layouts saved by the derived-objects prototype

Restore fixtures in the `<on_demand>` format of the prototype that preceded the
carved on-demand engine. Read by `main_window_release41_layout_test.cpp`
(`MainWindowPrototypeLayoutTest.RestoresLayoutSavedByPrototype`, GAP-39).

| File | Written by |
|---|---|
| `source_bound.pj4.xml` | `MainWindow::saveLayoutToPath(path, /*include_data_source=*/true)` |
| `generic.pj4.xml` | `MainWindow::saveLayoutToPath(path, /*include_data_source=*/false)` |
| `history_snapshot.xml` | `MainWindow::xmlSaveState(SnapshotScope::kHistory)` (an undo snapshot) |

Source commit: `766b20eeb` (branch `spike/pull-vs-ring`, built against the SDK at
`b40bf62`, 0.36.0), written by the real save paths of that build
(`MainWindow.cpp:6845`, the `<on_demand>` block). Only the source-bound file is
read by a test; the other two are kept as the prototype wrote them.

## The session

One source-bound dataset, `fixture-a.mcap` (dataset id 1), under the fake absolute
root `/pj-fixtures/data/`: a two-sample scalar `threshold` and a point cloud `cloud`
at 5 s. Five on-demand recipes, each reading `cloud` and `threshold` with one point
cloud output `cropped` unless noted:

- `fixture-toolbox/crop`, with the `label` "Crop box";
- `fixture-toolbox/downstream`, reading `fixture-toolbox/crop/cropped` (the
  prototype saved it before its producer), `query_time="0"`;
- `fixture-toolbox/finding`, pinned at 5 s;
- `fixture-assistant/exempt`, history-exempt;
- `fixture-toolbox/inferred`, with `infer_outputs="1"`.

The carved format writes neither `label` nor `infer_outputs`, and reads both
without effect.

## Regenerating

The writer is `prototype_fixture_writer.cpp.txt`, a throwaway gtest that is never
committed on the prototype. Its build needs the SDK tree the prototype was made
against (`git -C <sdk> archive b40bf62 | tar -x -C <dir>`, passed as
`PJ_SDK_LOCAL_DIR`). The data file must exist while saving (a `<fileInfo>` is
written only for an existing file), so the session points at an empty file under a
temporary root and the writer replaces that root with `/pj-fixtures`.

```bash
cp prototype_fixture_writer.cpp.txt <worktree>/pj_app/tests/prototype_fixture_writer.cpp
```

Register it in `<worktree>/pj_app/CMakeLists.txt`, after `pj_app_gui_tests`:

```cmake
pj_add_test_runner(prototype_fixture_writer
    MAIN gui
    SOURCES tests/prototype_fixture_writer.cpp tests/support/gui_test_env.cpp
    LIBS pj_app_shell pj_resources
)
target_include_directories(prototype_fixture_writer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
```

```bash
cmake build && cmake --build build --target prototype_fixture_writer -j16
PJ_FIXTURE_OUT_DIR=<out> QT_QPA_PLATFORM=offscreen build/pj_app/prototype_fixture_writer
```

Copy the three files from `<out>` here, then remove the writer and restore
`CMakeLists.txt` in the worktree.
