# PJ4 top-level `docs/`

Cross-cutting documentation: designs that span modules, build and deployment guides, and research notes. A module's
own docs live in `<module>/`: its `CLAUDE.md` (purpose, invariants, key headers, tests) and, when warranted,
`docs/ARCHITECTURE.md` (how it works today, ending with its known gaps) and `docs/TECHNICAL_NOTES.md` (what was
observed or tried, under which conditions). The root [`CLAUDE.md`](../CLAUDE.md) lists the modules and sets the
documentation rules.

## Designs

| Path | Contents |
|---|---|
| [`derived_objects/`](./derived_objects/README.md) | Derived series and objects: functions over scalar and object topics, evaluated on demand. Start at `README.md`; the design is `ARCHITECTURE.md`. |
| [`RECORDING_AND_CACHING.md`](./RECORDING_AND_CACHING.md) | The session recorder and the source cache: intent, architecture, trade-offs and rejected alternatives. |
| [`MCP_SERVER.md`](./MCP_SERVER.md) | AI agents over MCP: the in-app server, its stdio bridge and the `pj.mcp_endpoint.v1` plugin endpoint, jobs, tokens, security and tests. |
| [`visibility_gated_updates_plan.md`](./visibility_gated_updates_plan.md) | Open proposal: stop updating docks the user cannot see (maximize-hidden siblings, the scene streaming seam), with the catch-up on show it needs. |

## Guides

| Path | Contents |
|---|---|
| [`BUILDING.md`](./BUILDING.md) | Prerequisites, Linux and Windows builds, tests, build internals, versions, portability, third-party dependencies. Packaging formats: [`packaging/README.md`](../packaging/README.md). |
| [`SANITIZERS.md`](./SANITIZERS.md) | The `none` / `asan` / `tsan` / `msan` lanes: selectors, build trees, coverage, runtime defaults and the suppression policy. |
| [`QT_NOTES.md`](./QT_NOTES.md) | The pinned Qt (`PJ_QT_VERSION` in [`versions.env`](../versions.env)) and what changed since 6.8: new APIs, deprecations, build floors. Read before using an unfamiliar Qt API. |
| [`TELEMETRY.md`](./TELEMETRY.md) | The anonymous launch ping and the daily usage report: the exact field lists (enforced by `telemetry_ping_test` and `usage_reporter_test`), opt-outs, guards and the server contract. Update together with any payload change. |
| [`WASM_DEPLOYMENT.md`](./WASM_DEPLOYMENT.md) | The browser build: build and packaging, built-in plugins, notices and source offer, browser Python, the HTTP contract, browser-local state, presets, local serving and test fixtures. |
| [`WASM_PRESETS.md`](./WASM_PRESETS.md) | Browser presets: the layout contract, streaming sources, authoring, `package_wasm.mjs --presets` and what it refuses, tests, hosting. |
| [`WASM_HOSTING.md`](./WASM_HOSTING.md) | Serving the browser app from a robot over HTTPS with Caddy ([`wasm/Caddyfile`](./wasm/Caddyfile)), which also proxies the PlotJuggler Bridge at `/bridge`. |

## Research

Findings and measurements with the conditions they were taken under; versions and pins quoted in them are those of
the time, and the code and manifests are authoritative.

| Path | Contents |
|---|---|
| [`research/rerun_notes.md`](./research/rerun_notes.md) | Rerun's 2D architecture compared with pj_scene2D, and which of its ideas PJ took. |
| [`research/dataset_format_comparison.md`](./research/dataset_format_comparison.md) | MCAP, RLDS, LeRobot and Zarr compared. |
| [`research/mosaico_canonical_objects.md`](./research/mosaico_canonical_objects.md) | How Mosaico data reaches the ObjectStore as canonical objects without a parser, and how the scene modules decode and render it. |
| [`research/pixi_build_exploration.md`](./research/pixi_build_exploration.md) | Building PJ4 from pixi / conda-forge: the mapping, the local recipes, the CMake shims, the gotchas and the CI legs. |

`images/` and `plotjuggler_banner.svg` are the README's pictures.
