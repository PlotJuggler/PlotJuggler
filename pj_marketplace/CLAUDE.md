# pj_marketplace

## Purpose

Extension marketplace for PlotJuggler 4 — registry fetching, package download,
and installed-extension lifecycle management, plus the marketplace UI. Three
library targets (`pj_marketplace` core, `pj_marketplace_ui`, `pj_plugin_catalog`)
and a standalone `pj_marketplace_app` harness.

**Core (bundled) extensions**: plugins shipped with the app are seeded into the
extensions dir by the host (`pj_runtime`'s `ExtensionCatalogService` — the
bundled dir is a seed source, never a load path); the marketplace receives the
bundled id → version map (`setBundledVersions`), locks their uninstall in
default sessions, and offers downgrade-to-bundled. See
`docs/ARCHITECTURE.md` §4.4 and `docs/REQUIREMENTS.md` §4.4.

## Layout

- `include/pj_marketplace/` — public headers (`marketplace.hpp`, `registry_manager.hpp`,
  `registry_resolver.hpp`,
  `download_manager.hpp`, `extension_manager.hpp`, `installed_extension.hpp`,
  `marketplace_window.hpp`, `extension_detail_dialog.hpp`, `platform_utils.hpp`,
  `qt_diagnostic_bridge.hpp`).
- `src/core/` — registry/download/extension managers plus the pure registry candidate
  resolver; `src/ui/` — `MarketplaceWindow`
  + `ExtensionDetailDialog` (`.ui`-driven); `tests/` — manager and store suites share
  the `pj_marketplace_tests` gtest runner and its offscreen-capable `QApplication` main.

Consumed by the app through `pj_runtime`'s `ExtensionCatalogService`.

`gtest_discover_tests` registers each runner case as
`pj_marketplace_tests.<suite>.<case>` and runs it in its own process.
`plugin_check_runner_test` stays separate because it checks admission-helper
process exit results. `extension_manager_test` keeps its mock-target guard,
custom settings identity, and entry point because it re-executes itself in
lock-holder mode for the interprocess store lease test; `pj-plugin-check` is
copied beside that executable for default helper lookup. In a configured test
tree, select all registered module tests with
`ctest -R '^(pj_marketplace_tests\.|plugin_check_runner_test$|extension_manager_test$)'`
inside the builder image. `extension_manager_check_plugin_management` remains
a standalone, network-dependent harness excluded from CTest.

## Read deeper

| For | Read |
|---|---|
| Module purpose + Conan deps | [`README.md`](./README.md) |
| What it must do | [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md) |
| How it works | [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) |
| End-user flow | [`docs/USER_MANUAL.md`](./docs/USER_MANUAL.md) |
| Registry/package wire format | [`docs/plotjuggler-marketplace-spec-v1.0.0-en.md`](./docs/plotjuggler-marketplace-spec-v1.0.0-en.md) |
