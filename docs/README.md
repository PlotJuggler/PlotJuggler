# PJ4 top-level `docs/`

This folder is for **cross-cutting documentation only** — material that doesn't fit inside any single module's `docs/` folder.

> **Qt toolchain:** PJ4 builds against the Qt version pinned by `PJ_QT_VERSION` in
> [`../versions.env`](../versions.env). Install it with
> [`../scripts/install_qt6.sh`](../scripts/install_qt6.sh). See
> [`QT_NOTES.md`](./QT_NOTES.md) for what changed since 6.8.

Examples of what belongs here:

- Porting strategy / migration notes that span multiple modules.
- Glossary or terminology references used across the repo.
- Architecture decision records (ADRs) for choices that affect multiple modules.
- **Research notes** that informed multiple modules' designs — under `research/`.
- Work-in-progress design specs for modules that don't yet have their own `docs/`.
- Retired plans retained for historical context — under `archive/`.

## What does NOT belong here

- **Module-local intent docs** (REQUIREMENTS, ARCHITECTURE, USER_MANUAL, …) — those live in `<module>/docs/`.
  Examples in this repo: `pj_scene2D/docs/`, `pj_marketplace/docs/`.
- **Agent-management state** (`.remember/`, `.claude/`, `.agents/`) — those are not human-readable docs.

## Current contents

| Path | Status | Notes |
|---|---|---|
| [`BUILDING.md`](./BUILDING.md) | Guide | Build prerequisites, Linux/Windows setup, tests, and links to packaging and browser build instructions. |
| [Packaging guides](../packaging/README.md) | Guide | AppImage and Docker builds, Debian packages, and Windows installers. |
| [`SANITIZERS.md`](./SANITIZERS.md) | Guide | The `none` / `asan` / `tsan` / `msan` build lanes: selectors, build trees, test coverage, runtime defaults and the suppression policy. |
| `archive/PJ4_PLAN.md` | Retired | Original master plan; current architecture lives in root and module `CLAUDE.md` / `docs/`. Ongoing recording and cache work lives in the documents below. |
| `archive/QRHI_MIGRATION.md` | Retired | The completed QRhi migration of Scene3D, the plots and the media viewer (phases P0–P5, per-stream measurements, the Gate A sign-off and the final Gate A run against the GL renderer). The as-built design is in `pj_scene3D/docs/ARCHITECTURE.md` and `TECHNICAL_NOTES.md`; the remaining work in `pj_scene3D/docs/QRHI_NEXT.md`. |
| [`archive/derived_objects/SCENARIOS.md`](./archive/derived_objects/SCENARIOS.md) | Retired | The scenario catalog that mapped the derived-objects prototype's tests to behaviours. Not maintained; its row IDs (GAP-02, STR-05, ...) are still cited by tests. |
| `research/mosaico_canonical_objects.md` | Reference | Mosaico canonical-object ingestion, decoding, and rendering across the datastore and scene modules. |
| `QT_NOTES.md` | Reference | **Baseline for the pinned Qt.** Features new since 6.8 (likely past most models' training cutoff), deprecations, and build/platform floors. Read before using an unfamiliar Qt API or reaching for a 6.8-era workaround. |
| `TELEMETRY.md` | Reference | The anonymous launch ping: exact field list (the privacy contract, enforced by `telemetry_ping_test`), opt-out, and where it's implemented. Update together with any payload change. |
| `WASM_DEPLOYMENT.md` | Reference | Build, deterministic packaging, cross-origin isolation, compression negotiation, MIME, and cache requirements for the threaded browser application. |
| `WASM_PRESETS.md` | Guide | Browser presets: the layout contract, authoring a preset, `package_wasm.mjs --presets` and what it refuses, the sample tests, and the hosting limits and procedure. |
| `WASM_HOSTING.md` | Guide | Serving the browser app from a robot over HTTPS with Caddy ([`wasm/Caddyfile`](./wasm/Caddyfile)), which also proxies the PlotJuggler Bridge at `/bridge`: setup, certificates, robot presets with `${deployment.ws_origin}/bridge`, troubleshooting and the spec that runs the file. |
| `update_channels_design.md` | Design (delivered) | **How a release reaches an already-installed machine.** Why the apt repository and the Windows update repository are hosted on Cloudflare R2 rather than a metered package service, the publish orderings that keep index and payload consistent, accepted trade-offs (signing key in CI, no delta updates), and what is verified versus merely reviewed. |
| `undo_redo_integrity_plan.md` | Plan (delivered) | Consolidated undo/redo state-integrity delivery plan: dataset identity, transactional undo, timeline-state snapshots, processor/scene persistence — five incremental PRs (#373 #377 #379 #380 #382, all merged) distilled from two prior solution branches. |
| `was_pending_followup_plan.md` | Plan (draft) | Follow-up to the state-integrity epic: unified was-pending semantics for scene restore queues — serialize binding state instead of origin so exact-restore correctness is structural rather than shell-maintained. |
| `research/dataset_format_comparison.md` | Reference | Cross-cutting comparison of MCAP, RLDS, LeRobot, Zarr. Informs pj_scene2D and any future dataset-format work. |
| `research/rerun_notes.md` | Reference | Analysis of Rerun's 2D architecture; comparison input for pj_scene2D and (potentially) pj_scene3D. |
| `research/pixi_build_exploration.md` | Reference | Findings from building PJ4 with pixi / conda-forge instead of Conan: what maps one to one, the local recipes under `recipes/`, the CMake portability fixes and the semantic deltas. The pins quoted in it are those of the exploration; `pixi.toml` and `versions.env` are authoritative. |
| `visibility_gated_updates_plan.md` | Plan (partly delivered) | Not updating widgets the user cannot see. The active-tab gate on the playhead fan-out is in; the fuller `isVisible()` gate, the show-time catch-up and the floating-dock enumeration fix are the open part of the plan. |
| [`derived_objects/`](./derived_objects/) | Reference | **Derived objects:** functions over scalar and object topics, evaluated on demand. Start at [`README.md`](./derived_objects/README.md) (the engines and the path from a script to a layer); [`ARCHITECTURE.md`](./derived_objects/ARCHITECTURE.md) is the design as built, with its known limits (§20) and recipes for common changes (§21); [`REQUIREMENTS.md`](./derived_objects/REQUIREMENTS.md) holds the decisions D1-D30. |
| `RECORDING_AND_CACHING.md` | Reference | **Recording and caching, the durable spec.** Intent, goals, architecture, accepted trade-offs and rejected alternatives (plugin-side caching, provider-driven cache ABI, parser config in the file, promote-at-stop) for the session recorder and the source cache. Read this first; `session_recorder_design.md` is the working design behind it. |
| `session_recorder_design.md` | Design (M1 shipped) | Host-side raw recording of delegated-ingest messages into MCAP: stream Record/Stop (M1, shipped as `RecordingService`), then the connector source cache on the same core and the Mosaico transport/parser split (status in `RECORDING_AND_CACHING.md`). Records the 2026-08-31 decisions (raw only, subscribed topics, bounded backpressure, global+per-source, auto-named folder). |

## Where to find module docs

See the "Module documentation index" in the top-level [`CLAUDE.md`](../CLAUDE.md). Each PJ4 module owns a `CLAUDE.md` at its root and, when warranted, a `docs/` folder with `REQUIREMENTS.md` / `ARCHITECTURE.md`.
