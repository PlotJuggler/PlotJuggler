# pj_marketplace technical notes

What was observed or reasoned behind the current code, with the conditions and when to look again. How the module works
is in [ARCHITECTURE.md](./ARCHITECTURE.md).

## dlopen pins the first image

- **Observed (glibc, Linux):** plugin DSOs export `STB_GNU_UNIQUE` symbols, so `dlclose` never unloads them, and a
  second `dlopen` of the same path name returns the image loaded first. The `extension_manager_test` fixtures that pin
  (the pinning plugins) reproduce it. glibc version not recorded.
- **Consequence:** opening an extension directory before a staged update replaces it makes every later scan in that
  process report the old version, and the marketplace re-offers an update that is already on disk. Opening a payload at
  its staging path pins that image and makes the check at its final path read the wrong bytes.
- **What the code does:** pending work is drained before the first scan (`ExtensionManager` construction); a backup is
  named without opening the old directory; the duplicate-directory cleanup runs after the whole promotion loop;
  admission of fresh bytes and of promoted directories runs out of process (`pj-plugin-check`); the version the startup
  seed wrote is reported through `setSeededVersions()` because no in-process rescan can see it; removal and
  replacement are staged to the next launch so a directory name is never reused inside one process.
- **Re-test:** when the loader or the plugin build changes (no unique symbols), or if plugins stop being opened
  in-process at all.

## Removal records outside the payload

- **Observed:** the first removal scheme kept its marker inside the directory being deleted. `QDir::removeRecursively()`
  deletes children before it can fail on the one it cannot unlink, so a removal that lost part-way (a mapped DSO on
  Windows) had already destroyed its own retry record: the payload survived and nothing said it should go. A Windows CI
  run exposed it; the test that reproduces it now runs on every platform.
- **What the code does:** the journal is a sibling directory of the extensions dir; a record is validated against the
  canonical store root before any delete (see "Removal journal" in ARCHITECTURE).
- **Re-test:** not applicable while the journal stays outside the payload.

## QSettings status belongs to the writing instance

- **Reasoning, not measured:** `QSettings::status()` reflects only the object that performed the write; on Windows a
  registry failure surfaces on that instance's flush. A sync-and-status check through another instance would report on
  an object that wrote nothing and lose the failure.
- **What the code does:** `ExtensionManager::writeDisabledState()` reads, writes, syncs and checks on a single
  `QSettings`, and staging a removal fails (with the enable state rolled back) when the entry cannot be persisted.
- **Re-test:** on a Qt upgrade, or if the disabled list moves out of `QSettings`.

## Cancel latency of a download

- **Reasoning, not measured:** extraction checks the cancel flag at every archive entry and every data block, so a
  cancel waits for about one block. The SHA-256 check is a single call and is not interruptible; it is assumed to be
  short next to the extraction for artifacts of tens of megabytes.
- **What the code does:** `DownloadManager::cancelAndWait()` and the destructor wait for the worker; nothing times
  them. The disk-space check (free space of at least three times the download size, tested once the size is known) is a
  heuristic of the same kind, not derived from measured expansion ratios.
- **Re-test:** when a cancel or a window close visibly stalls on a large artifact.

## Qt libraries for the admission helper

- **Observed:** `pj-plugin-check` is Qt-free but `dlopen`s every candidate, and a Widget plugin links Qt. Outside the
  AppImage (a development tree, a .deb) nothing tells the loader where `libQt6Widgets` lives, and the failed load was
  reported as a rejected plugin.
- **What the code does:** `PluginCheckRunner::childEnvironment()` puts Qt's library directory first on
  `LD_LIBRARY_PATH` (Linux) or `PATH` (Windows). macOS finds Qt through the helper's rpath.
- **Re-test:** when packaging gains an rpath for the helper, or Widget plugins stop linking Qt.

## libarchive differences the extractor accommodates

- `archive_entry_filetype()` returns `__LA_MODE_T`: `mode_t` only where POSIX provides it, unsigned short on Windows,
  int from libarchive 4.0. The extractor deduces the type instead of spelling it.
- A ZIP written without seeking defers entry sizes to a descriptor after the payload; libarchive then reports size
  zero when the central directory is out of the reader's reach. The extractor charges a declared size up front and
  bounds an undeclared one while the bytes arrive.
- On Windows libarchive normalizes `\` to `/` before the path reaches the policy, so the backslash refusal is
  Linux-shaped; and a hand-patched STORED entry that understates its size takes another path in the Windows build (the
  corresponding test is skipped there).
- **Re-test:** on a libarchive upgrade; the extraction tests in `download_manager_test` pin each case.

## TSan and QProcess exec failures

- **Observed:** with `cmake/sanitizers/tsan.supp` making calls from libQt6Core bypass TSan's interceptors, a `QProcess`
  whose child failed to exec blocks forever: Qt learns of the exit through a SIGCHLD handler that TSan runs only when the
  thread next passes through an interceptor. Hangs with that suppression entry, passes in about 5 ms without it. Qt and
  compiler versions not recorded.
- **What the code does:** the missing-helper case in `plugin_check_runner_test` is skipped under TSan.
- **Re-test:** when the suppression file or Qt changes.
