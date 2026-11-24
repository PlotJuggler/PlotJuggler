# Hosted browser app (`app.plotjuggler.io`)

The WebAssembly build is published at `https://app.plotjuggler.io` by a Cloudflare Worker
(`worker.mjs`, named `plotjuggler-app`; `plotjuggler-web` is the plotjuggler.io website's) serving
a deployment package from the R2 bucket `plotjuggler-web`. Cloudflare Pages cannot hold it:
`plotjuggler4.wasm` is larger than the 25 MiB per-file limit (`docs/WASM_PRESETS.md`, "Cloudflare Pages").

The Worker serves a package exactly as `scripts/serve_wasm.mjs` does locally, sharing its rules
(`scripts/wasm_http_contract.mjs`): only the files the package's `manifest.json` declares, with
the cross-origin isolation headers, the two cache classes, and the `.br` / `.gz` copies chosen by
`Accept-Encoding`. It also serves `sources/`, the GPL-3.0 source offer the bundle's notices point
to, `/presets/index.json` from the asset host's bucket ([Sample index](#sample-index)), and keeps the
telemetry Worker (`plotjuggler`, repository `PlotJuggler/pj-telemetry`) reachable:

- `stats.plotjuggler.io`: every request goes to the telemetry Worker (its dashboard and API);
- `/telemetry` on any host goes there too: installed desktop releases post their launch ping to
  `https://app.plotjuggler.io/telemetry`, so that path must keep working on this host.

## Releases

Each deployment is one release, uploaded once under `releases/<first 16 hex of the manifest's
SHA-256>/` and never changed. The Worker's `RELEASE` variable names the live one, so publishing or
rolling back is one Worker deploy and a page never mixes two releases.

```bash
# The production package of the revision to publish, with its sources (docs/WASM_DEPLOYMENT.md), without --presets:
node scripts/package_wasm.mjs --source build-wasm-prod/pj_app --output build-wasm-prod/deploy
node scripts/wasm_source_offer.mjs --links --output build-wasm-prod/deploy/sources

# The sample index the Worker serves; deploy.mjs refuses without it. Skip it while the bucket's copy is current
# (an entry that needs this release goes up only after the production deploy, see "Sample index"):
node packaging/web/upload_preset_index.mjs

# A preview on plotjuggler-app-preview.<account>.workers.dev (no custom domain), then production:
node packaging/web/deploy.mjs --package build-wasm-prod/deploy --env preview
PJ_WASM_PACKAGE_DIR=build-wasm-prod/deploy PJ_WASM_DEPLOYMENT_URL=<preview url> npm --prefix tests/wasm run test:deployment
node packaging/web/deploy.mjs --package build-wasm-prod/deploy --env production
```

`deploy.mjs` runs the pinned wrangler (`wrangler.mjs`) through `npx`, logged in to the PlotJuggler account, refuses a
production release without `sources/SOURCE-OFFER.txt` and any deploy while the sample index cannot be read from the
bucket, and `--dry-run` prints what it would do. Old releases stay in the bucket, and any of them can be deployed
again; delete one only when no deployment can need it again.

`wrangler.toml` declares `app.plotjuggler.io` and `stats.plotjuggler.io` as this Worker's custom domains. A
domain attached to another Worker in the dashboard (for instance `app.plotjuggler.io` on the telemetry Worker
`plotjuggler`: Workers → plotjuggler → Domains) must be detached before this Worker's deploy can take it;
installed releases' pings to `/telemetry` fail only in between.

## Sample index

The welcome dialog's samples come from `presets/index.json` beside the page, which the Worker answers from the
object `presets/index.json` of the asset host's R2 bucket `plotjuggler-assets` (binding `ASSET_HOST`), beside the
layouts, thumbnails and recordings it names; the asset host serves the same object at
`https://assets.plotjuggler.io/presets/index.json`. The Worker reads it at every request and never from the
release, so a `presets/` inside a package (one built with `--presets`) is shadowed and harmless: the answer is
`application/json; charset=utf-8`, `Cache-Control: no-cache` and R2's ETag, so a browser revalidates and gets a 304
while it is unchanged. Without the object the route is a 404 and the app starts with no samples (a `?preset=` link
reports that the page has no such preset); an R2 error is an uncached 503. Preview and production read the same
object.

That object is the production index; `resources/wasm-presets/index.json` is its source in the repository, which the
tests and self-hosted packages use. Change the repository copy, then upload it:

```bash
node packaging/web/upload_preset_index.mjs --dry-run   # validates, prints the wrangler command
node packaging/web/upload_preset_index.mjs             # uploads resources/wasm-presets/index.json
```

It refuses, before uploading, what `package_wasm.mjs --presets` refuses (`docs/WASM_PRESETS.md`, "Packaging"),
and also any relative `layout` or `thumbnail`, since nothing is deployed beside it, and any `http:` one, which an
https page blocks as mixed content. It takes another index file as its argument. Adding or listing a sample is then: upload its
files to the asset host, upload the index. Upload an index entry only once the deployed app supports it (a layout
feature, a key such as `listed`): the live app reads the index at once, and preview and production share it.

Tests: `node --test packaging/web/*.test.mjs` (also in `npm --prefix tests/wasm run test:package`); the Worker's
index route in a published deployment is checked by `test:deployment` (`docs/WASM_DEPLOYMENT.md`).
