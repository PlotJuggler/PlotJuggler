# PJ4 top-level `docs/`

This folder is for **cross-cutting documentation only** — material that doesn't fit inside any single module's `docs/` folder.

> **Qt toolchain:** PJ4 builds against the Qt version pinned in
> [`../versions.env`](../versions.env) (currently **Qt 6.11.1**). Install it with
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
| [Packaging guides](../packaging/README.md) | Guide | AppImage and Docker builds, Debian packages, and Windows installers. |
| `archive/PJ4_PLAN.md` | Retired | Original master plan; current architecture lives in root and module `CLAUDE.md` / `docs/`. Ongoing recording and cache work lives in the documents below. |
| `research/mosaico_canonical_objects.md` | Reference | Mosaico canonical-object ingestion, decoding, and rendering across the datastore and scene modules. |
| `QT_NOTES.md` | Reference | **Qt 6.11.1 baseline.** Features new since 6.8 (likely past most models' training cutoff), deprecations, and build/platform floors. Read before using an unfamiliar Qt API or reaching for a 6.8-era workaround. |
| `TELEMETRY.md` | Reference | The anonymous launch ping: exact field list (the privacy contract, enforced by `telemetry_ping_test`), opt-out, and where it's implemented. Update together with any payload change. |
| `WASM_DEPLOYMENT.md` | Reference | Build, deterministic packaging, cross-origin isolation, compression negotiation, MIME, and cache requirements for the threaded browser application. |
| `undo_redo_integrity_plan.md` | Plan (delivered) | Consolidated undo/redo state-integrity delivery plan: dataset identity, transactional undo, timeline-state snapshots, processor/scene persistence — five incremental PRs (#373 #377 #379 #380 #382, all merged) distilled from two prior solution branches. |
| `was_pending_followup_plan.md` | Plan (draft) | Follow-up to the state-integrity epic: unified was-pending semantics for scene restore queues — serialize binding state instead of origin so exact-restore correctness is structural rather than shell-maintained. |
| `research/dataset_format_comparison.md` | Reference | Cross-cutting comparison of MCAP, RLDS, LeRobot, Zarr. Informs pj_scene2D and any future dataset-format work. |
| `research/rerun_notes.md` | Reference | Analysis of Rerun's 2D architecture; comparison input for pj_scene2D and (potentially) pj_scene3D. |
| `RECORDING_AND_CACHING.md` | Reference | **Recording and caching, the durable spec.** Intent, goals, architecture, accepted trade-offs and rejected alternatives (plugin-side caching, provider-driven cache ABI, parser config in the file, promote-at-stop) for the session recorder and the source cache. Read this first; `session_recorder_design.md` is the working design behind it. |
| `session_recorder_design.md` | Design (M1 implemented, `feat/session-recorder-m1`) | Host-side raw recording of delegated-ingest messages into MCAP: stream Record/Stop first (M1), then the connector source cache on the same core and the Mosaico transport/parser split. Records the 2026-08-31 decisions (raw only, subscribed topics, bounded backpressure, global+per-source, auto-named folder). |

## Where to find module docs

See the "Module documentation index" in the top-level [`CLAUDE.md`](../CLAUDE.md). Each PJ4 module owns a `CLAUDE.md` at its root and, when warranted, a `docs/` folder with `REQUIREMENTS.md` / `ARCHITECTURE.md`.
