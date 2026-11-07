# WebAssembly presets: samples, packaging and hosting

A **preset** is an ordinary PlotJuggler layout that records its sources, deployed
beside the browser application so a visitor can open it with one click (the
welcome dialog) or a link (`?preset=<id>`). This guide covers what a preset must
contain, how to author one, how `scripts/package_wasm.mjs` deploys a preset tree,
and what hosting the package needs. How the browser discovers presets and the
dialog behaves is in [`WASM_DEPLOYMENT.md`](./WASM_DEPLOYMENT.md#presets); the
samples shipped with the repository, their provenance and their licenses are in
[`resources/wasm-presets/README.md`](../resources/wasm-presets/README.md).

## The preset contract

A preset tree is a directory with an `index.json` and one folder per preset:

```text
presets/
├── index.json
└── waveforms/
    ├── layout.pj4.xml
    ├── data.csv
    └── thumbnail.png
```

`index.json` lists the presets in display order; `layout` and `thumbnail` resolve
against the index URL:

```json
{"presets": [{"id": "waveforms", "title": "Waveforms", "description": "...",
              "layout": "waveforms/layout.pj4.xml", "thumbnail": "waveforms/thumbnail.png"}]}
```

The layout is what **File → Save Layout with sources** writes
(`<root binding="source">`), with each data file beneath the layout's folder:

- `<fileInfo filename="data.csv" content_sha256="…">` — the filename is relative
  to the layout's URL, or an absolute `https://` URL on an asset host that sends
  CORS headers (as the ANYmal sample's recording), and the download must match `content_sha256` when the
  layout carries one (a mismatched or malformed fingerprint fails the whole
  load). Browser-saved layouts write a fingerprint; a desktop **Save Layout with
  sources** does not, so a desktop-authored preset is only size-capped. A
  pinned recording is also kept by each visitor's browser, so a revisit loads
  it without the network (`docs/WASM_DEPLOYMENT.md`); an unpinned one downloads
  on every visit.
- `<plugin ID="…" manifest_id="…" filepath_mode="source">` — every replay rewrites
  the plugin configuration's `filepath` to the file it loads (here, the downloaded
  file), so the same layout also opens on the desktop next to its data. The marker
  itself exists for older 4.x builds, which rewrite only when it is present.
- A robot whose URDF arrives on a topic resolves `package://` meshes through
  `<robot_model package_url_roots='{"pkg":"rel/dir/"}'>`, relative to the
  layout, or an absolute `https://` root on an asset host. Keep each package's
  `package://pkg/...` structure under that folder. An asset host keeps large
  meshes out of the repository and the package (PlotJuggler's own is
  `https://assets.plotjuggler.io/`, Cloudflare R2, immutable `v1/` prefixes).

The index, a layout and a thumbnail are capped at 16 MiB each
(`kMaxPresetDownloadBytes`), a preset's data files at 512 MiB each and together
(`kMaxPresetDataBytes`). A recording streams straight into the page's in-memory
file system, outside the WebAssembly heap, but it stays in the browser's memory
next to what the app loads from it, so keep a sample's recording well below
that. Robot meshes are fetched by Scene3D under its own
per-file model budget (32 MiB in the browser, `kBrowserSceneLimits.max_model_source_bytes`).
Presets are same-origin by default; one on another origin, and an asset host
serving meshes, needs CORS (`Access-Control-Allow-Origin`) on every file (see
[`WASM_DEPLOYMENT.md`](./WASM_DEPLOYMENT.md#required-http-contract)).

## Authoring a preset

1. Load the data in PlotJuggler (desktop or browser), arrange the views, and
   save with **File → Save Layout with sources**. A browser save of a single
   source writes the bare file name; of several, `pj4-sources/<n>/<name>` — put
   each file at that path beside the layout. A desktop save writes a path
   relative to the layout when the data lives beneath its folder.
2. Remove anything machine-specific: absolute paths, `file://` package roots.
   For a topic URDF add `package_url_roots` to its `<robot_model>`.
   Omitting a plot's `<range>` makes it fit its data on open.
3. Add a thumbnail: a small PNG (the dialog shows 192x108; the samples use
   384x216, a few tens of KB), ideally a render of the loaded preset.
4. Add the entry to `index.json`, package (below) and open it through the
   reference server.

Every path segment must match `[A-Za-z0-9_-][A-Za-z0-9._-]*`, and every file
must have a deployable type (extension in any case): `.xml`, `.json`, `.csv`,
`.mcap`, `.png`, `.md`, `.txt`, `.stl`, `.dae`, `.obj`, `.gltf`, `.glb`, or no
extension (served as text, for `LICENSE`/`NOTICE`).

### Streaming sources

A layout can also record streaming sources beside its files, one `<streamInfo>`
per stream:

```xml
<root binding="source" pj4_version="5">
  <previouslyLoaded_StreamingSources>
    <streamInfo id="robot" source_name="Robot">
      <plugin ID="PlotJuggler Bridge" manifest_id="plotjuggler-bridge"><![CDATA[
        {"url":"${deployment.ws_origin}/bridge"}
      ]]></plugin>
      <dataset display_offset_ns="0" timeline_order="0"/>
    </streamInfo>
  </previouslyLoaded_StreamingSources>
</root>
```

- `id` is required and unique within the layout. It only tells two streams of
  the same plugin apart: views bound to a stream carry the qualifier
  `pj-stream://<id>` (for example `dataset_path="pj-stream://robot"`). It is not
  a connection key; every load starts its own sessions.
- `<plugin>` names the plugin by `manifest_id` (matched first) and `ID` (the
  display name, the fallback); at least one is required. The CDATA payload is the
  plugin's saved configuration and must not be empty. `<dataset>` (Source
  Timeline offset and order) is optional.
- One invalid record (no id, a repeated id, no plugin or two, no configuration),
  or a second `<previouslyLoaded_StreamingSources>` block, rejects every stream
  with an error.
- A layout carrying this block is written with `pj4_version="5"`; layouts without
  it stay at 4, so an older PlotJuggler warns only about layouts whose streams it
  would ignore.

`${deployment.ws_origin}` lets one layout work wherever the app is served from.
Inside a JSON string value of the configuration it becomes the WebSocket origin
of the page that opened the app: `http://robot.local:8080/app/` gives
`ws://robot.local:8080`, `https://[fd00::2]:8443/` gives `wss://[fd00::2]:8443`
(host, IPv6 brackets and port kept, no trailing slash, so append the path:
`${deployment.ws_origin}/bridge`). It expands only in string values, never in
keys, and is an error anywhere else. `${deployment.` is reserved: any other name
in it (a typo such as `${deployment.wsorigin}`), or a token without its closing
`}`, is an error that names the token. Any other `${...}` belongs to the plugin
and is left alone. On the desktop, or on a page that is not
http(s), there is no page origin: the variable is a "missing deployment
context" error, so a layout meant for the desktop names an explicit URL
(`"ws://10.0.0.2:9090/bridge"`), which works everywhere unchanged. Saving the
layout again keeps the variable, never the expanded address.

Opening the layout (a preset, or a browser upload) starts every stream before
its views apply, in the same all-or-nothing load as its files:

- The configuration, its deployment variables filled in, goes straight to the
  plugin's `loadConfig()`: no dialog opens, and the plugin's remembered settings
  are neither read nor overwritten. A plugin that refuses it, a plugin this build
  lacks, or a `start()` that fails ends the load with reason `stream`; a
  variable that cannot be filled in ends it with `stream-config`, and an invalid
  record block with `stream-records`, both before anything downloads. Every
  failure removes what the load imported or started and gives the previous
  workspace back.
- Each record starts a new session with a new dataset, also when another stream
  of the same plugin, or of the same layout, is already running. What the layout
  qualified by `pj-stream://<id>` binds to that record's session and waits there:
  a curve or a scene layer draws once its own stream publishes the topic, never
  a same-named topic of another stream.
- A failure after the stream started (a connection lost later) is reported as
  for any stream.
- The views wait for their stream's topics, but data processors (filters,
  transforms, marker rules, custom topics) do not: they restore with the layout
  and need their inputs then, so a stream preset's processors must not read a
  stream's topics yet. One that does fails the load like any layout that cannot
  apply, and gives the previous workspace back.

A robot's stream is usually a PlotJuggler Bridge (built into the browser app). Its
configuration needs only `url`, the bridge's full `ws://` or `wss://` URL, path
included and used as written (`{"url":"ws://10.0.0.2:9090/bridge"}`). Without
`url` it connects to `ws://<address>:<port>` (`address` and `port` default to
`127.0.0.1` and `9090`); a `url` that is not a `ws://`/`wss://` string is
refused (reason `stream`). The load does not wait for the connection: an
unreachable bridge fails the stream as a stream error, at once when the port
refuses, about 5 s later when nothing answers. Once connected it subscribes the
topics the layout's views show, as they ask for them; it also briefly previews
the other advertised topics to learn their fields, then releases them. An
optional `topics` list (`[{"name": "/topic"}]`) narrows the topics it offers, and
so what it previews. Bridge topics show without their leading slash
(`robot/counter/value`). A page served over HTTPS reaches only `wss://` bridges.
A robot that serves the app itself, as in [`WASM_HOSTING.md`](./WASM_HOSTING.md),
forwards `/bridge` on its own HTTPS origin, so `{"url":"${deployment.ws_origin}/bridge"}`
works on every robot.

In the browser build, **File → Save Layout with sources** writes one record per
running stream (the desktop save writes none), with the template it started
from (an interactively started stream gets the configuration its dialog
produced, under the id `stream`), its Source Timeline offset and slot, and
`pj-stream://<id>` on everything drawn from it. The plain
**Save Layout**, browser storage and undo/redo never contain stream records, and
undo/redo never starts a stream. On the desktop, only a layout opened by URL
starts its streams; a layout file opened from disk warns and ignores them.

## Packaging

```bash
node scripts/package_wasm.mjs --source build-wasm/pj_app --output build-wasm/deploy \
  --presets resources/wasm-presets
```

(`packageWasm({ presets, ... })` from Node.) The tree is copied under `presets/`
and every file is declared in `manifest.json` with its MIME type, the `revalidate`
cache class (`no-cache`: a sample may change under the same name, unlike the
content-addressed bundle) and its representations, each with size and SHA-256.
The identity file is the tree's own bytes. Text and mesh types (extensionless
files such as `LICENSE`, `.csv`, `.dae`, `.gltf`, `.json`, `.md`, `.obj`,
`.stl`, `.xml`, in any letter case) of at least 1 KiB also get `.br` and `.gz`
siblings with the bundle's fixed settings (a 27 MB set of COLLADA robot
meshes compresses to 7 MB). `.mcap` (zstd chunks), `.png` and `.glb` (embedded textures)
are compressed inside and stay identity-only. The browser decodes
`Content-Encoding` before the app reads a file, so a source's `content_sha256`
is over the decoded bytes and holds for whichever representation is served.
`_headers` gains a `/presets/*` `Cache-Control: no-cache` rule. Packaging stays
byte-deterministic (CI packages twice and runs `diff -qr`).

The packager refuses, before writing anything:

- an **unusable `index.json`** — missing, malformed, without a `presets` array,
  with an entry the browser rejects (which empties the whole welcome list: an
  entry that is not an object, a missing, empty or repeated `id`, no `layout`, or
  an `id`/`title`/`description`/`layout`/`thumbnail` that is not a string), or
  naming a relative `layout`/`thumbnail` the tree does not deploy (absolute
  http(s) references pass: they live elsewhere);
- **symbolic links** — they can point outside the tree, to a relative `..` or
  an absolute target, so they are refused rather than followed;
- **names outside the segment rule** — dotfiles and dot folders (`.git`,
  `.DS_Store`, editor state, which no static host should publish), and
  URL-significant or non-portable characters (`?`, `#`, `%`, `\`, spaces);
- **special files** (sockets, FIFOs, devices) and **types with no MIME mapping**;
- a preset tree that contains the output directory or lies inside it, since
  replacing the output deletes it.

The reference server (`scripts/serve_wasm.mjs`) serves exactly what the manifest
declares, so a preset needs no server change.

## Testing

`tests/wasm/package_wasm.test.mjs` covers the tree declaration, the refusals and
determinism (`npm --prefix tests/wasm run test:package`). The production
deployment contract checks every deployed preset file's bytes, MIME and cache
policy and that the production build fetches `presets/index.json` at boot
(`test:deployment` on a package built with `--presets`).

The acceptance specs boot a package without presets, so no welcome dialog sits
over their window. The sample cases in `tests/wasm/presets.spec.js` serve a
second acceptance package that carries the samples, `build-wasm-probe/deploy-presets`
(override with `PJ_WASM_PRESETS_PACKAGE_DIR`):

```bash
node scripts/package_wasm.mjs --source build-wasm-probe/pj_app \
  --output build-wasm-probe/deploy-presets --presets resources/wasm-presets
npm --prefix tests/wasm run test -- presets.spec.js -g sample
```

`-g sample` selects the sample cases. The rest of `presets.spec.js` boots the
plain acceptance package, which must be served at `PJ_WASM_URL` like every
other spec.

`bridge.spec.js` runs a fake bridge, `tests/wasm/support/bridge_server.py`, which
needs Python 3.14+ or an older Python with the `zstandard` package
(`PJ_BRIDGE_PYTHON` picks the interpreter). `robot_deployment.spec.js` puts the same
fake bridge behind Caddy and `docs/wasm/Caddyfile` (see
[`WASM_HOSTING.md`](./WASM_HOSTING.md#testing)).

They open every sample into its views (the ANYmal robot with all its meshes, its
compressed lidar scan, its three camera views and its joint, IMU and speed plots;
the self-driving car's camera, lidar following the car and motion plots; the
built-in filters sample's raw and moving-average plots, its filter restored from
the layout), check that seeking
between two points inside the recording changes what the 3D view draws (a scene
drawn once and then frozen fails), reopen ANYmal after switching sample while
watching the page's longest frame gap, and make loads fail after staging
and after importing to check that the staged files are released and the open
sample is left as it was. The suites never reach the network, so the samples get
their hosted recordings and models from a local mirror of
`https://assets.plotjuggler.io`: `npm --prefix tests/wasm run fetch:assets` fills
`build-wasm-assets/` (`PJ_WASM_ASSETS_MIRROR` moves it), checking every file
against `tests/wasm/fixtures/hosted_assets.json`; a hosted file a sample adds
goes into that manifest.

Known behaviour the samples show:

- Opening a sample replaces what the sample opened before it brought — its
  files and its streams — once the new one has loaded; data the visitor opened,
  a sample dataset the visitor merged, and streams no preset started stay. A sample that fails to open leaves the
  previous one as it was.

## Hosting

The package is a static site with the response contract of
[`WASM_DEPLOYMENT.md`](./WASM_DEPLOYMENT.md#required-http-contract): HTTPS,
`Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy:
require-corp`, `Cross-Origin-Resource-Policy: same-origin`, `nosniff`, the
manifest's MIME types and cache classes, and ideally the precompressed `.br` /
`.gz` representations chosen by `Accept-Encoding`. **No public host is selected**;
account provisioning, DNS, credentials and publication are outside this feature. A robot
serving the app beside its bridge uses the Caddy recipe in
[`WASM_HOSTING.md`](./WASM_HOSTING.md).

### Cloudflare Pages (reference candidate)

Cloudflare Pages is the reference candidate because it reads the generated
`_headers`. Its published limits
([Pages limits](https://developers.cloudflare.com/pages/platform/limits/),
[headers](https://developers.cloudflare.com/pages/configuration/headers/); re-check
them before publishing) are 25 MiB per file, 20,000 files per deployment on the
Free plan, and 100 `_headers` rules of at most 2,000 characters a line.

**`plotjuggler4.wasm` exceeds the 25 MiB per-file limit** (it is roughly twice
that), so the package cannot be uploaded to Pages as it is. Everything else fits
by a wide margin: its `.br` / `.gz` copies and every sample file are under the
limit, the file count is about a hundred, and `_headers` holds a handful of short
rules. Pages also compresses on its own instead of selecting the `.br` / `.gz`
files, which it would publish as ordinary files. Hosting there needs the module
served from a host without that limit (with `application/wasm`, CORS and CORP
for the page) or a smaller module. To check a package against the limit:

```bash
find build-wasm-prod/deploy -type f -size +25M
```

### Upload and header procedure (any static host)

1. Build and package with `--presets` as above, then run
   `test:deployment` against the package.
2. Upload the package directory as it is, keeping paths; never strip
   `assets/sha256-…/` or rename files the manifest declares.
3. Configure the response headers from `_headers` (or the host's equivalent):
   the four isolation headers on every path, `no-cache` on `/`, `/index.html`,
   `/manifest.json`, `/THIRD_PARTY_NOTICES.txt` and `/presets/*`,
   `public, max-age=31536000, immutable` on the bundle, and the manifest's
   `contentType` per file. Serve the `.br` / `.gz` representations by
   `Accept-Encoding` with `Vary: Accept-Encoding` when the host can; otherwise
   let it compress the identity files.
4. Smoke-test the deployed site:

```bash
BASE=https://<host>/<path>   # no trailing slash
# The bare URL must serve the app or redirect to the trailing-slash URL: the page URL's
# directory is what `presets/index.json` resolves against.
curl -sI "$BASE" | grep -iE '^(HTTP|location)'
curl -fsSI "$BASE/" | grep -iE '^(cross-origin|cache-control|content-type)'
curl -fsSI "$BASE/presets/index.json" | grep -iE '^(cross-origin|cache-control|content-type)'
curl -fsS "$BASE/presets/index.json" | node -e 'JSON.parse(require("fs").readFileSync(0))'
```

Then open `$BASE/?preset=builtin_filters` in Chromium and Firefox: its two plots appear,
and the browser console shows no errors.
