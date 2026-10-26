# Hosted browser app (`app.plotjuggler.io`)

The WebAssembly build is published at `https://app.plotjuggler.io` by a Cloudflare Worker
(`worker.mjs`, named `plotjuggler-app`; `plotjuggler-web` is the plotjuggler.io website's) serving
a deployment package from the R2 bucket `plotjuggler-web`. Cloudflare Pages cannot hold it:
`plotjuggler4.wasm` is larger than the 25 MiB per-file limit (`docs/WASM_PRESETS.md`, "Cloudflare Pages").

The Worker serves a package exactly as `scripts/serve_wasm.mjs` does locally, sharing its rules
(`scripts/wasm_http_contract.mjs`): only the files the package's `manifest.json` declares, with
the cross-origin isolation headers, the two cache classes, and the `.br` / `.gz` copies chosen by
`Accept-Encoding`. It also serves `sources/`, the GPL-3.0 source offer the bundle's notices point
to, and keeps the telemetry Worker (`plotjuggler`, repository `PlotJuggler/pj-telemetry`)
reachable:

- `stats.plotjuggler.io`: every request goes to the telemetry Worker (its dashboard and API);
- `/telemetry` on any host goes there too: installed desktop releases post their launch ping to
  `https://app.plotjuggler.io/telemetry`, so that path must keep working on this host.

## Releases

Each deployment is one release, uploaded once under `releases/<first 16 hex of the manifest's
SHA-256>/` and never changed. The Worker's `RELEASE` variable names the live one, so publishing or
rolling back is one Worker deploy and a page never mixes two releases.

```bash
# The production package of the revision to publish, with its sources (docs/WASM_DEPLOYMENT.md):
node scripts/package_wasm.mjs --presets resources/wasm-presets --source build-wasm-prod/pj_app --output build-wasm-prod/deploy
node scripts/wasm_source_offer.mjs --links --output build-wasm-prod/deploy/sources

# A preview on plotjuggler-app-preview.<account>.workers.dev (no custom domain), then production:
node packaging/web/deploy.mjs --package build-wasm-prod/deploy --env preview
PJ_WASM_PACKAGE_DIR=build-wasm-prod/deploy PJ_WASM_DEPLOYMENT_URL=<preview url> npm --prefix tests/wasm run test:deployment
node packaging/web/deploy.mjs --package build-wasm-prod/deploy --env production
```

`deploy.mjs` runs its pinned wrangler through `npx` (logged in to the PlotJuggler account), refuses a production
release without `sources/SOURCE-OFFER.txt`, and `--dry-run` prints what it would do. Old releases stay in the
bucket; delete one only when no deployment can need it again.

The first production deploy moves `app.plotjuggler.io` off the telemetry Worker, which holds it as a
custom domain attached in the dashboard (its own `wrangler.toml` declares no route, so its redeploys
do not take it back). Detach it from `plotjuggler` first (Workers → plotjuggler → Domains), then
deploy; installed releases' pings to `/telemetry` fail only in between.

Tests: `node --test packaging/web/worker.test.mjs`.
