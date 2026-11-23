# pj_marketplace

Extension marketplace for PlotJuggler 4: registry fetching, package download, the installed-extension lifecycle
(install, sideload, update, uninstall, enable/disable) and the marketplace UI. Targets: `pj_marketplace` (core),
`pj_marketplace_ui` (`MarketplaceWindow`), the Qt-free `pj-plugin-check` admission helper and the standalone
`pj_marketplace_app` harness. Embedded-manifest discovery comes from the SDK (`plotjuggler_sdk::plugin_host`). The app
consumes it through `pj_runtime`'s `ExtensionCatalogService`, which also seeds the bundled (core) plugins. How it works:
[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md).

## Invariants

- Updates, uninstalls and downgrades are staged to the next launch on every platform; a loaded DSO is never replaced or
  deleted in place. dlopen resolves by path name and glibc keeps the first image of a path, so reusing a name inside a
  process serves stale bytes (docs/TECHNICAL_NOTES.md "dlopen pins the first image").
- Never open an extension or staging directory in-process before the startup drain that may replace it, and admit fresh
  bytes through `PluginCheckRunner`, or the process pins the old image and reports old versions.
- One writer per store across processes: every store mutation checks `hasStoreWriteAccess()`; staging siblings (install
  transactions, removal journal, lease, content-addressed store) derive from `PlatformUtils::canonicalStoreRoot()` and
  sit beside the extensions dir, never inside it, or the plugin scan finds scratch and two aliases of one store take two
  leases.
- A removal-journal record authorises a recursive delete: validate it fully, confine it to a direct child of the
  canonical store root, and never let the drain enable an id (the uninstall tombstone stays), or a corrupt record can
  delete outside the store or make a removed plugin loadable.
- The disabled list is persisted and its sync checked before a removal is journalled, on one `QSettings` instance
  (`writeDisabledState`), or a failed deletion lets the removed plugin load again.
- The registry pruning rule in `pj-plugin-registry` depends on admission being monotonic in `min_sdk_required` (no upper
  bound, no ABI check on registry records, platform filter first): change admission rules there first, or released hosts
  lose releases they could load (ARCHITECTURE, "Registry and compatibility").
- `min_plotjuggler_version` is displayed and never gates; the gate is the ABI major plus `min_sdk_required`, evaluated by
  the one `evaluatePluginCompatibility`.
- Paths in messages, logs and JSON go through `utf8_path.hpp`, never `path::string()` or `path(qstring.toStdString())`
  (the Windows code page throws or mangles).
- Build it only as part of the PJ4 tree: `pj_marketplace_ui` links `pj_widgets`, which a standalone configure of this
  directory does not provide.

## Key headers (`include/pj_marketplace/`)

- `extension_manager.hpp`: the lifecycle, staged promotion, the removal journal, the writer lease, diagnostics.
- `registry_manager.hpp`, `registry_resolver.hpp`: fetch and parse, then the pure candidate selection.
- `download_manager.hpp`, `archive_limits.hpp`: HTTP, checksum, extraction under size, count and depth budgets.
- `plugin_compatibility.hpp`, `version_compare.hpp`, `plugin_check_runner.hpp`: admission gate, version ordering, the
  out-of-process helper.
- `store_session.hpp`, `store_lease.hpp`, `artifact_store.hpp`, `loaded_profile.hpp`, `content_manifest.hpp`,
  `generation_digest.hpp`, `store_policy.hpp`, `store_rejection.hpp`, `stored_file.hpp`: the lease and the
  content-addressed store.
- `sha256.hpp`, `fs_walk.hpp`, `utf8_path.hpp`, `platform_utils.hpp`: reusable helpers.
- `extension.hpp`, `installed_extension.hpp`: a registry release and an installed one.
- `marketplace_window.hpp`, `qt_diagnostic_bridge.hpp`, `marketplace.hpp` (aggregate include): UI and diagnostics glue.

## Tests

`./test.sh -R '^(pj_marketplace_tests\.|plugin_check_runner_test$|extension_manager_test$)'`. `gtest_discover_tests`
registers each `pj_marketplace_tests` case as its own process. `plugin_check_runner_test` stays separate because it
checks the helper's process exit results; `extension_manager_test` keeps its own `main` because it re-executes itself
in lock-holder mode for the interprocess lease test, and `pj-plugin-check` is copied beside it for the default helper
lookup. `PJ_MARKETPLACE_TESTING` exposes test hooks on `ExtensionManager`. The runner starts the helper with Qt's
library directory first on the loader path (a Widget plugin links Qt: docs/TECHNICAL_NOTES.md "Qt libraries for the
admission helper"). `extension_manager_check_plugin_management` is a network-dependent harness outside CTest.

A test's store root is a `PJ::test::StoreTempDir` (`cmake/test_mains/pj_test_store_dir.h`), never a bare
`QTemporaryDir`: the store keeps its siblings (`.artifacts`, `.extraction`, `.profiles`, `.state`, `.install_stage`,
the lease file) beside the root, and beside a temp dir's own path they leak into TMPDIR.

## Docs

- [docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md): components, registry and compatibility, install and update, removal
  journal, single-writer lease, content-addressed store, admission, bundled extensions, disk layout, behaviour
  contracts, UI integration, known gaps.
- [docs/TECHNICAL_NOTES.md](./docs/TECHNICAL_NOTES.md): observations behind the code (dlopen pinning, journal placement,
  Qt libraries for the helper, libarchive, TSan).
- [docs/USER_MANUAL.md](./docs/USER_MANUAL.md): the end-user flow.
- [docs/plotjuggler-marketplace-spec-v1.0.0-en.md](./docs/plotjuggler-marketplace-spec-v1.0.0-en.md): the registry and
  package formats shared with `pj-plugin-registry` and the extension repositories.
