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
  + `ExtensionDetailDialog` (`.ui`-driven); `tests/` — one gtest binary per manager.

Consumed by the app through `pj_runtime`'s `ExtensionCatalogService`.

## Read deeper

| For | Read |
|---|---|
| Module purpose + Conan deps | [`README.md`](./README.md) |
| What it must do | [`docs/REQUIREMENTS.md`](./docs/REQUIREMENTS.md) |
| How it works | [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md) |
| End-user flow | [`docs/USER_MANUAL.md`](./docs/USER_MANUAL.md) |
| Registry/package wire format | [`docs/plotjuggler-marketplace-spec-v1.0.0-en.md`](./docs/plotjuggler-marketplace-spec-v1.0.0-en.md) |
