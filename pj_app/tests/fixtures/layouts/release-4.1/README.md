# Layouts saved by release-4.1

Restore fixtures in the format users of release-4.1 have on disk. Read by
`main_window_release41_layout_test.cpp` (`MainWindowRelease41LayoutTest.*`).

| File | Written by |
|---|---|
| `source_bound.pj4.xml` | `MainWindow::saveLayoutToPath(path, /*include_data_source=*/true)` |
| `generic.pj4.xml` | `MainWindow::saveLayoutToPath(path, /*include_data_source=*/false)` |
| `history_snapshot.xml` | `MainWindow::xmlSaveState(SnapshotScope::kHistory)` (an undo snapshot) |

Source commit: `a5796f385` (release-4.1, "perf(markers): copy a marker rule's input
series in linear time (#710)"), written by the real save paths of that build.

## The session

Two source-bound datasets, `fixture-a.mcap` (dataset id 1, display offset +2.5 s)
and `fixture-b.mcap` (dataset id 2, offset −1.25 s), under the fake absolute root
`/pj-fixtures/data/`. Scalar topics `/sensor` and `/imu` on A, `/sensor` and `/odom`
on B, five samples each at 1..5 s; a point cloud `/points` on A at 1 s.

- Transforms: `fixture-toolbox/t1` (`/imu` × 10, params `{"gain":10}`),
  `fixture-toolbox/t2` (negates `t1_scaled`), `fixture-assistant/t3` (negates
  `/odom`, history-exempt).
- Filters (`absolute`): `imu_abs` on A's `/imu`, `t1_abs` on the transform output
  `t1_scaled`.
- Marker generators: `fixture-toolbox/rule/a-sensor` (on A, params),
  `fixture-toolbox/rule/all` (all datasets), `fixture-toolbox/rule/t1` (reads
  `t1_scaled/value`), `fixture-toolbox/rule/declared` (declared input
  `fixture-a.mcap:/sensor/value`), `fixture-assistant/rule/odom` (on B,
  history-exempt).
- Plots: tab `tab1` with `imu_abs`, `t1_scaled`, `t2_negated`, A's `/sensor`; tab
  `fixture-derived` with `t1_abs`, `t3_exempt`, B's `/sensor`, and B's all-datasets
  marker scope hidden.
- Tab `fixture-scene`: one Scene3D dock showing A's `/points`.

Not producible on release-4.1, so not in the fixtures: a generator window
(release-4.1's `GeneratorRecipe` has none), and a scene dock mixing datasets
(`SceneDockWidget::addLayer` already refuses a second dataset there).

As released, a generator's `dataset_path` is absolute in every file, the generic
layout included. Later builds make it relocatable like the other qualifiers and strip
a generator's dataset qualifiers from a generic layout, so the generic resave check
expects the fixture with that strip applied.

## Regenerating

The writer is `release41_fixture_writer.cpp.txt`, a throwaway gtest that is never
committed on release-4.1. The data files must exist while saving (a `<fileInfo>` is
written only for an existing file), so the session points at empty files under a
temporary root and the writer replaces that root with `/pj-fixtures` in what it
writes. The test reverses that: it creates the files under its own temporary root and
substitutes it for `/pj-fixtures` before loading.

```bash
git -C <worktree> switch --detach a5796f385
cp release41_fixture_writer.cpp.txt <worktree>/pj_app/tests/release41_fixture_writer.cpp
```

Register it in `<worktree>/pj_app/CMakeLists.txt`, after `pj_app_gui_tests`:

```cmake
pj_add_test_runner(release41_fixture_writer
    MAIN gui
    SOURCES tests/release41_fixture_writer.cpp tests/support/gui_test_env.cpp
    LIBS pj_app_shell pj_resources
)
target_include_directories(release41_fixture_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

```bash
cmake build && cmake --build build --target release41_fixture_writer -j8
PJ_FIXTURE_OUT_DIR=<out> QT_QPA_PLATFORM=offscreen build/pj_app/release41_fixture_writer
```

Copy the three files from `<out>` here, then remove the writer and restore
`CMakeLists.txt` in the worktree.
