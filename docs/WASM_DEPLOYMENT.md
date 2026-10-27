# PlotJuggler 4 WebAssembly deployment

PlotJuggler's browser build is a static, threaded Qt/WebAssembly application.
It must be served over HTTPS (or localhost during development) with
cross-origin isolation enabled. Opening `index.html` directly from disk or
using an ordinary `python -m http.server` is not a supported threaded setup:
the packaged entrypoint then shows why on the page ("PlotJuggler cannot start:
this page was not served over HTTPS." or "…is not cross-origin isolated.", with
what the server must do) instead of loading the runtime. When the server does send
the headers and the page still is not isolated, the browser itself cannot isolate
it (an Android WebView, as in DuckDuckGo), and the page says to open another one. To serve the app from
a robot together with its PlotJuggler Bridge, see
[`WASM_HOSTING.md`](./WASM_HOSTING.md).

Qt's deployment guidance describes the generated HTML, JavaScript loader, and
Wasm module, recommends gzip or Brotli compression, and documents the headers
needed by a multithreaded build:
[Qt for WebAssembly](https://doc.qt.io/qt-6/wasm.html). Emscripten separately
requires `application/wasm` for streaming compilation and COOP/COEP for
Pthreads:
[WebAssembly server setup](https://emscripten.org/docs/compiling/WebAssembly.html#web-server-setup),
[Pthreads support](https://emscripten.org/docs/porting/pthreads.html).

## Build and package

The exact release toolchain is pinned in `versions.env`. Scene2D video
cross-compiles FFmpeg and dav1d at build time, which needs `meson`, GNU `make`
and `pkg-config` on the host (`-DPJ_WASM_WITH_VIDEO=OFF` drops video and these
requirements). Browser Python builds CPython at configure time, which needs a
native C compiler, `make`, `patch` and `node` (`-DPJ_WASM_WITH_PYTHON=OFF`
drops it; see below). A local equivalent of the release workflow is:

```bash
source /path/to/emsdk/emsdk_env.sh
embuilder build zlib   # Scene2D resolves Emscripten's sysroot zlib port
cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/Qt/<PJ_QT_VERSION>/wasm_multithread/lib/cmake/Qt6/qt.toolchain.cmake \
  -DQT_HOST_PATH=/path/to/Qt/<PJ_QT_VERSION>/gcc_64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DPJ_BUILD_TESTS=OFF -DPJ_BUILD_DEMOS=OFF \
  -DPJ_WASM_PYTHON_CACHE_DIR="$HOME/.cache/pj-wasm-cpython"
cmake --build build-wasm --target pj_app --parallel 4

node scripts/package_wasm.mjs \
  --source build-wasm/pj_app \
  --output build-wasm/deploy \
  --presets resources/wasm-presets
```

A public deployment also publishes the source offer
([licence and source offer](#licence-of-the-browser-bundle-and-the-source-offer)):
`node scripts/wasm_source_offer.mjs --links --output build-wasm/deploy/sources`.

`--presets` is optional: it deploys a preset tree (the repository's samples
here) under `presets/`; see [Presets](#presets).

### Built-in plugins

The browser cannot `dlopen` extensions, so a fixed set of official plugins is
compiled from a pinned `pj-official-plugins` commit (`cmake/PjWasmDependencies.cmake`)
and registered statically by `pj_app/src/BuiltinPlugins.cpp`. Each has a
`cmake/PjWasm<Name>.cmake` recipe and an option, all `ON` by default:

| Option | Plugin |
|---|---|
| `PJ_WASM_WITH_CSV` | `data_load_csv` |
| `PJ_WASM_WITH_MCAP` | `data_load_mcap` |
| `PJ_WASM_WITH_3D_LOADER` | `data_load_3d` (PCD/PLY) |
| `PJ_WASM_WITH_DUMMY_STREAM` | `data_stream_dummy` |
| `PJ_WASM_WITH_PJ_BRIDGE` | `data_stream_pj_bridge` (over the browser's WebSocket) |
| `PJ_WASM_WITH_ROS` | `parser_ros` |
| `PJ_WASM_WITH_PROTOBUF` | `parser_protobuf` |
| `PJ_WASM_WITH_JSON` | `parser_json` |
| `PJ_WASM_WITH_DATA_TAMER` | `parser_data_tamer` |
| `PJ_WASM_WITH_TRANSFORM_EDITOR` | `toolbox_transform_editor` |
| `PJ_WASM_WITH_QUATERNION` | `toolbox_quaternion` |
| `PJ_WASM_WITH_FFT` | `toolbox_fft` |
| `PJ_WASM_WITH_ANOMALY_DETECTOR` | `toolbox_anomaly_detector` (the host runs its rules) |

Visualization families are on by default too: `PJ_WASM_WITH_SCENE2D` (images,
depth, annotations, and with `PJ_WASM_WITH_VIDEO` software H.264/HEVC/AV1 video
from a statically linked LGPL FFmpeg) and `PJ_WASM_WITH_SCENE3D`.

Plugins that need raw sockets, child processes or native middleware (UDP, ZMQ,
MQTT, LSL, ROS 2, the AI assistant) cannot run in a browser. The PlotJuggler
Bridge needs none of them: it connects with the browser's own WebSocket, so a page
served over HTTPS reaches only `wss://` bridges (mixed content), and the browser
reports a failed connection without its cause. Its stream presets are described
in [`WASM_PRESETS.md`](./WASM_PRESETS.md#streaming-sources). Adding another
plugin needs a recipe, a link block in `pj_app/CMakeLists.txt` and a
registration in `BuiltinPlugins.cpp`; its export macros must name an
unqualified class (or use the `_NAMED` variants) so the static getter symbol is
valid. Every new static archive also needs the symbol-collision audit described
in the root `CLAUDE.md`.

The packaging command refuses missing, invalid, or single-threaded build
outputs. To prevent a mistyped `--output` from deleting unrelated data, it
only replaces an absent directory, an empty directory, or a prior package with
a valid PlotJuggler deployment manifest. It emits a stable entrypoint and
content-addressed assets. The entrypoint is PlotJuggler's own page,
`packaging/web/loading.html.in`, not Qt's generated `plotjuggler4.html`: it
streams the module into `WebAssembly.compileStreaming` through a byte counter
from the moment the page runs (beside the loader scripts), hands the compiled
module to Qt's loader (`qt.module`) once the page has loaded, and shows the progress
against the decoded module size the packager writes into it (the browser counts
decoded bytes, while `Content-Length` is the compressed size). A failed start
shows its reason and a Reload button. On a small touch screen (coarse pointer,
shorter screen side under 600 CSS px) the page first says PlotJuggler is not
intended for mobile devices and fetches nothing until "Continue anyway".

For a `?preset=<id>` deep link the page also prefetches that preset's
recordings while the app boots. It reads `presets/index.json` and the preset's
layout, and once the module has downloaded (so boot keeps the link to itself) a
dedicated Worker fetches every `<fileInfo filename>` that is an absolute
http(s) URL, one after the other, within the app's 512 MiB
(`kMaxPresetDataBytes`), buffering each body in ~8 MB pieces. `BrowserFetch`
takes a prefetched response from `globalThis.pjPrefetch` instead of fetching
that URL, so a recording crosses the network once (a second request for a URL
still downloading stalls on Chromium's HTTP-cache lock), and reads it through
the same loop as its own downloads: the SHA-256 pin, size limit, idle timeout
and progress apply unchanged, and its cancel aborts the prefetch. A prefetch
that fails leaves the app to fetch as usual; one the app has not taken 60 s
after it started is dropped. Nothing else is prefetched: no deep link, an
unknown id, the welcome dialog's choices, a recording the browser already keeps.

The browser keeps every downloaded recording whose `<fileInfo>` carries a
`content_sha256` pin, so a returning visitor loads it without the network (the
HTTP cache refuses bodies this large). It lives in Cache Storage
(`pj-recordings-v1`, no Service Worker), keyed on the pin and the URL
(`/pj-recording-cache/<sha256>?source=<url>`): a changed pin or URL simply
misses, and entries never change. `BrowserFetch` reads a kept copy through the
same loop as a download, so the pin check, size limit and progress apply; a
copy that no longer matches its pin is refused (`reason=fingerprint`) and
dropped, and the next load downloads it again. A download is kept only after it
matched its pin, streamed from its MEMFS file without a second copy. The kept
recordings stay under 1 GiB together: storing one first drops the
longest-kept ones (a hit does not refresh an entry), and nothing is stored when
`navigator.storage.estimate()` leaves no room for it. Storage is best effort:
the page does not ask for persistent storage (Firefox would prompt), so the
browser may evict the cache under pressure, and any cache error, private mode
or full quota just means downloading again. The loading page does not prefetch
a kept recording.

```text
build-wasm/deploy/
├── index.html
├── manifest.json
├── _headers
├── THIRD_PARTY_NOTICES.txt[.br|.gz]
├── assets/sha256-<bundle-id>/
│   ├── plotjuggler4.js[.br|.gz]
│   ├── plotjuggler4.wasm[.br|.gz]
│   ├── qtloader.js[.br|.gz]
│   ├── plotjuggler.svg[.br|.gz]   (the page icon)
│   └── THIRD_PARTY_NOTICES.txt[.br|.gz]
└── presets/            (with --presets: the tree, text and mesh files also as [.br|.gz])
```

`manifest.json` records every identity/Brotli/gzip path, byte size, SHA-256,
MIME type, cache class, and compression runtime/parameters. Gzip metadata is
normalized, Brotli uses a fixed quality/window, and release CI pins Node, so
identical inputs produce byte-identical packages. The generated `_headers`
file's path, size, and hash are covered separately as deployment configuration;
the file configures isolation and cache policy on hosts that
support the Netlify/Cloudflare Pages format; it does not make a host select
precompressed variants automatically.

### Third-party notices

`plotjuggler4.wasm` links every library statically. `cmake/PjWasmNotices.cmake`
writes `THIRD_PARTY_NOTICES.txt` beside `plotjuggler4.js` at configure time: one
section per library with its SPDX licence expression and its licence text,
copied from its source (Qt's modules, plugins and bundled third-party code from
the kit's SBOM, `sbom/*.spdx.json`, followed by the text of every standard
licence they name, one file each in `cmake/wasm_notices/<SPDX id>.txt`; the
configure fails, naming the id, when one is missing), and CPython's notices when
Python is on.
`package_wasm.mjs` refuses a build without the file. `npm run test:package`
(`tests/wasm/wasm_notices.test.mjs`) fails when a `FetchContent_Declare` of the
WASM build has no `pj_wasm_notice()` entry. What that test cannot see is kept
by hand in the same file: a Qt module or plugin added to the link
(`_PJ_QT_NOTICE_PACKAGES`), a vendored directory or font, third-party code
bundled inside a dependency (assimp's `contrib/`, the plugins' `contrib/`,
rosx_introspection's `contrib/`), and the Emscripten runtime. The About dialog's
"Third-party licences" link (browser build only) opens `THIRD_PARTY_NOTICES.txt`
by that relative name, so the package serves a copy at the deployment root
(`no-cache`), beside the immutable one in the bundle directory;
`tests/wasm/package_wasm.test.mjs` fails if the link and the package disagree.

### Licence of the browser bundle and the source offer

The browser build (`plotjuggler4.wasm`, `plotjuggler4.js`, `qtloader.js`) is a
combined work conveyed under the GNU GPL version 3, because it statically links
Qt's WebAssembly platform plugin, which Qt provides only under the GPL v3 or a
commercial licence. PlotJuggler 4's own source is MPL-2.0, which permits this
combination; the desktop application remains MPL-2.0 (its Qt is LGPL, shared).
The libraries linked statically, none replaceable at run time, keep their own
licences:

| Library | Licence | Source |
|---|---|---|
| Qt (Core, Gui, Widgets, Network, OpenGL, OpenGLWidgets, PrintSupport, Xml, Concurrent, Svg, SvgWidgets, UiTools, image/TLS plugins) | LGPL-3.0-only | the official prebuilt `wasm_multithread` kit, unmodified |
| Qt's WebAssembly platform plugin (qwasm) | GPL-3.0-only or commercial (the kit's SBOM, `QWasmIntegrationPlugin`) | the same kit |
| FFmpeg (libavcodec, libavutil, libswscale) | LGPL-2.1-or-later, built `--disable-gpl --disable-nonfree` | the SHA-256-pinned tarball of `cmake/PjWasmFfmpeg.cmake`, unmodified |
| Qt Advanced Docking System | LGPL-2.1-or-later | PlotJuggler's fork, the `3rdparty/Qt-Advanced-Docking` submodule |

Qwt (LGPL-2.1 with the Qwt exception, which addresses static linking) is in
the offer like every other fetched dependency. The header of `THIRD_PARTY_NOTICES.txt` (written in
`cmake/PjWasmNotices.cmake`) states the bundle's licence, points at the GPL-3.0
text in its Qt section, and says, as the desktop packages'
`packaging/deb/copyright` does, that `sources/SOURCE-OFFER.txt` next to the
deployment names the complete corresponding source and where each part is, and
that it is available on written request via the PJ4 issue tracker. No object files are published; PJ4's source pins the toolchain and the
commands to rebuild the module against a modified library.

`scripts/wasm_source_offer.mjs` writes that `sources/` directory. Run it after
`package_wasm.mjs` (which replaces its output), from a clean checkout of the
built revision, and upload the result with the deployment:

```bash
node scripts/package_wasm.mjs --source build-wasm/pj_app --output build-wasm/deploy
node scripts/wasm_source_offer.mjs --links --output build-wasm/deploy/sources   # ~17 MB, needs network
```

`--links`, the mode `app.plotjuggler.io` publishes with (`packaging/web/`),
archives only PlotJuggler 4, whose repository is private, and gives every other
part its public address: the URL and SHA-256 of a hashed pin, the commit and
GitHub tarball of a git pin, the Qt Advanced Docking System fork's commit. The
GPL v3 accepts source on another server when the object code carries clear
directions to it, as long as it stays available there: every address is
fetched once and the run fails if one does not answer, and the pinned commits
of repositories PlotJuggler controls (the docking-system fork,
`pj-official-plugins`, `plotjuggler_sdk`, `cloudini`, `rosx_introspection`)
must never be deleted. Without `--links` it copies everything, as below.

It takes a `git archive` of PlotJuggler 4 at `HEAD` (its vendored `3rdparty/`
libraries included) and of the pinned Qt Advanced Docking System commit,
downloads `qtbase`, `qtsvg` and `qttools` at `PJ_QT_VERSION` from download.qt.io,
and archives every other library the build fetches: each `FetchContent_Declare`
pin of `cmake/PjWasm*.cmake`, the root `CMakeLists.txt` (the SDK) and
`3rdparty/qwt`, the CPython tarball of `PjWasmPython.cmake`, and the Abseil that
protobuf fetches in its own build (at the `abseil-cpp-version` of the protobuf
commit's `cmake/dependencies.cmake`, read from GitHub). It writes `SOURCE-OFFER.txt` (each
archive with its URL, SHA-256 or commit, and how to rebuild with a modified
library). A `URL_HASH` is verified and the run fails on a mismatch or on a pin
without one; Qt's hashes are in the script (`QT_SHA256`, from the `.sha256`
files beside the archives), so a Qt bump needs its three hashes added there. A
`GIT_TAG` pin has no hash: the tag is resolved with `git ls-remote` to its
commit, which is archived as GitHub's tarball of that commit (the build's git
clone would also fetch submodules, so a tag whose repository lists any fails
the run), and the SHA-256 it has is recorded in `SOURCE-OFFER.txt`. A new
`FetchContent_Declare` needs no change here: `npm run test:package`
(`tests/wasm/wasm_source_offer.test.mjs`, no download) fails when a declaration
has no usable pin, and a dependency a build fetches another way needs a line in
`planSourceOffer`. `--dry-run` lists the files. The Emscripten runtime (musl,
libc++, compiler-rt, the zlib port) is not copied: it comes with the pinned
Emscripten SDK, which `SOURCE-OFFER.txt` names. Writing it takes about a minute
and 300 MB. The host must serve `sources/` as plain static files, the largest
about 60 MB; the reference server `serve_wasm.mjs` serves only manifest files, so
it does not. A host with a per-file limit (Cloudflare Pages: 25 MiB) cannot hold
`qtbase`, `assimp` or `draco`: `sources/` may then live on another origin (a
release asset, object storage), in which case a `SOURCE-OFFER.txt` with the
absolute URLs must sit at the deployment's `sources/SOURCE-OFFER.txt` path so the
pointer in the notices keeps working.

### Browser Python (`PJ_WASM_WITH_PYTHON`)

On-demand Python scripts run in the browser through a static CPython 3.14 for
Emscripten with pthreads. The option is `ON` by default, in the production
build too (derived-objects slice P-d); `-DPJ_WASM_WITH_PYTHON=OFF` builds the
app without it, and a recipe in Python then fails to restore with "python is
not available in this build". WASM CI's functional suite runs
`tests/wasm/python.spec.js` (the lane's limits, per-call cost and main-thread
gaps) and `tests/wasm/python_layout.spec.js` (a layout's Python recipes
restored into a 3D scene, a broken one showing its compile error) in Chromium
and Firefox. Python filters stay unavailable in the browser; what else remains
is in `docs/derived_objects/ARCHITECTURE.md` §14.

`cmake/PjWasmPython.cmake` builds CPython at configure time, from the SHA-256
pinned release tarball (its only download) and the files in
`cmake/wasm_python/`: `build_cpython.sh`, one patch for the two upstream files
PJ4 changes, the `Setup.local` that disables unused builtins, and
`stdlib_zip.py`, which writes the pyc-only stdlib. The host needs a native C
compiler, `make`, `patch` and `node` (CPython's cross build first builds a
native interpreter of the same release). A cold build takes about 4 minutes;
the result is a stage `stage-<key>` under `PJ_WASM_PYTHON_CACHE_DIR` (default
`<build>/_deps/pj_wasm_cpython`), the key a hash over the tarball, the
Emscripten release and every recipe file, so a reconfigure or a restored CI
cache skips it. A stage is published by a rename under a per-key lock, so
build trees can share the cache; stages of old keys stay until it is cleared.
The build runs in a cleared environment (only the emsdk variables pass), with a
fixed `SOURCE_DATE_EPOCH`, the work directory mapped out of the debug info and
configure run by a relative path: inherited flags cannot change it unseen, and
two cold builds in different directories give identical stages:

```bash
cmake ... -DPJ_WASM_WITH_PYTHON=ON -DPJ_WASM_PYTHON_CACHE_DIR="$HOME/.cache/pj-wasm-cpython"
```

What to know before changing it:

- **Size budget.** It adds about 1.9 MB to the Brotli download (2026-10-21,
  the acceptance configuration with and without it), about 12 % of the
  production package's 16.13 MB. CI fails the production package above
  16,932,000 B, 5 % over that measurement (`scripts/wasm_size_budget.mjs`; the
  sample `presets/` are fetched on demand and not counted): an absolute budget,
  since both configurations now carry Python and a delta would need a third
  build. About 0.67 MB of it is the stdlib zip, stored uncompressed
  so the package's Brotli compresses it with the module.
- **The stdlib zip** holds only the modules that importing the roots in
  `stdlib_zip.py` loads (68 of them). A module the stdlib imports
  lazily is missed unless a root names it, and even one in the zip cannot be
  imported inside a strict run, whose audit hook refuses the file open: the
  sandbox loads the ones a strict script reaches at boot
  (`python_sandbox.cpp`, `heapq` and decimal's `_pydecimal` fallback, on
  desktop too). The acceptance probe runs those calls in a strict run and
  walks what a strict script can reach, failing on each lazy import not
  loaded by then: the signal to add it to both lists.
- **Process-wide syscall.** CPython's `__syscall_ioctl` replaces Emscripten's
  JS syscall for the whole module, Qt included (FIOCLEX/FIONCLEX succeed,
  FIONBIO goes through `fcntl`); without it every Python `open()` fails.
- **Threads.** The pthread pool is 7 with Python. A worker created while the
  main thread waits on it never starts, so `main.cpp` starts the Python helper
  thread at startup: a validate waits for it, at most 100 ms. The main thread
  never compiles a recipe's Python (a script compiles at its first run, on the
  Python lane).
- **Notices.** The CPython stage writes its notices (the PSF-2.0 licence, its
  incorporated-software notices including libmpdec's BSD licence, and HACL*'s
  MIT licence), which end the bundle's `THIRD_PARTY_NOTICES.txt`.
- **Symbol audit** (rerun on every CPython or Emscripten bump). `llvm-nm
  --defined-only -g` over the CPython archives (3,352 globals), against the
  other 240 inputs of `pj_app`'s link line (113,891) and the 13 sysroot
  libraries the link pulls (6,243, from `-Wl,--trace`): no symbol is defined
  twice (2026-10-21). 58 CPython globals carry no `Py`/`mpd`/`Hacl` prefix
  (libmpdec's `crt3` and `fnt_*`, libpython's `fileio_spec`, `bytesio_spec`, …):
  those are the names a future archive could clash with.

## Required HTTP contract

Every response, including the entrypoint and runtime assets, needs:

| Header | Value |
| --- | --- |
| `Cross-Origin-Opener-Policy` | `same-origin` |
| `Cross-Origin-Embedder-Policy` | `require-corp` |
| `Cross-Origin-Resource-Policy` | `same-origin` |
| `X-Content-Type-Options` | `nosniff` |

Serve the logical asset URL using content negotiation:

- prefer its `.br` sibling when `Accept-Encoding` allows `br` and return
  `Content-Encoding: br`;
- otherwise use `.gz` with `Content-Encoding: gzip`, or the identity file;
- always keep the logical resource's MIME type—especially
  `Content-Type: application/wasm` for `plotjuggler4.wasm`; and
- return `Vary: Accept-Encoding` for resources with encoded variants.

The stable `index.html` and `manifest.json` use `Cache-Control: no-cache` so a
client revalidates for a new bundle. Files below `assets/sha256-<bundle-id>/`
use `Cache-Control: public, max-age=31536000, immutable`; their URL changes when
any runtime asset changes. A host may ignore the precompressed siblings and use
equivalent dynamic compression, provided the same MIME, isolation, and cache
contract is preserved.

Cross-origin data loaded by the application has its own CORS/CORP requirements.
Do not weaken the application asset policy globally to work around an unrelated
remote server; configure that data origin explicitly. A preset's layout, data
files and robot meshes count as such data when they live on another origin: it
must answer them with `Access-Control-Allow-Origin` for the page.

## Browser-local state

Uploaded files are staged in the page's temporary filesystem and receive opaque
browser identities; they are not persisted across refreshes. Ordinary
`QSettings` callers are likewise redirected to page-lifetime storage. The only
durable browser state is the typed preference allowlist and a bounded list of
source-free generic layout recipes owned by `BrowserPersistence`. Source-bound
layouts are downloaded with logical references and require explicit file
reselection when replayed. Paths, upload identities, plugin configuration, and
credentials are intentionally excluded from browser-local persistence.

A layout opened by URL (a preset) replays its `<fileInfo>` sources without a
picker instead. Each `filename` resolves against the layout's URL — the browser
reports the requested URL even after a redirect, so a preset must not rely on a
redirect to another directory — and only http(s) references are fetched: a local
path or another scheme, like a local robot model reference, fails the load.
Sources download one at a time, straight into their staged files (never through
the WebAssembly heap), must match their recorded `content_sha256`, and may stage
at most 512 MiB together (`kMaxPresetDataBytes`). They are staged
like uploads and imported all or nothing: any failure or cancel removes what the
load imported and restores the previous workspace. A layout's streaming sources
start in the same load, from their saved configuration and without a dialog
(see [`WASM_PRESETS.md`](./WASM_PRESETS.md#streaming-sources)). Once a preset
has loaded, the files and streams the previous preset brought are removed;
anything else stays. Saving that session again writes the usual logical
references, which a later open reselects.

## Presets

A deployment offers presets by publishing `presets/index.json` in the page's
directory: the page `https://host/app/index.html?preset=builtin_filters` reads
`https://host/app/presets/index.json` (the page's query and fragment play no
part). The page URL must end in `/` or a file name, because `presets/index.json`
resolves against it: `https://host/app` would read
`https://host/presets/index.json`. The index is
`{"presets": [{"id", "title", "description"?, "layout", "thumbnail"?}]}`, with
`layout` and `thumbnail` resolved against the index URL; the index, a layout and
a thumbnail are capped at 16 MiB each, a preset's data files at 512 MiB each and
together.

- **Presets listed:** once the window is up, a welcome dialog offers them (with
  their thumbnails) beside "Upload your own…", which opens the ordinary
  data-file picker; File → Presets reopens it. Picking one opens its layout as
  above, with Cancel while it loads (a bar shows how much of each data file
  has downloaded) and Retry if it fails. A thumbnail that does
  not download or decode shows a placeholder and leaves its preset usable.
- **No index, or an empty `presets` list:** the page starts exactly as it would
  without the feature: no dialog, no File → Presets, no message (the browser
  still logs the request's 404 in its own console).
- **Malformed index** (bad JSON, an entry without `id` or `layout`, a duplicate
  id, a reference that is not http(s)): the same, plus one console warning,
  `PJ_PRESET_INDEX_INVALID url=… reason=…`.
- **Deep link:** `?preset=<id>` opens that preset directly, the dialog showing
  its progress. An id the index does not list, the `preset` key given twice, or
  a deep link on a page without a usable index reports an error and loads
  nothing. Other query keys are ignored.

The repository ships three samples in `resources/wasm-presets/` (a car's gyro
through a built-in filter; an ANYmal walking a forest trail; a self-driving car
with its camera and lidar; their recordings and models load from
`https://assets.plotjuggler.io`), deployed with `--presets`. Preset files are served `no-cache`. How to author a preset,
what the packager refuses, and the hosting limits are in
[`WASM_PRESETS.md`](./WASM_PRESETS.md).

## Local serving and release verification

The repository includes a manifest-driven reference server for local testing:

```bash
node scripts/serve_wasm.mjs --root build-wasm/deploy --port 6931
```

It binds to `127.0.0.1` by default, performs real Brotli/gzip negotiation, sends
the production header/cache contract, and rejects files not declared by the
manifest. It is a correctness reference and development server, not a managed
TLS or high-availability production service.

The deployment acceptance test verifies raw encoded response hashes, MIME,
cache/isolation headers, conditional requests, and a fresh threaded Chromium
boot from the packaged root:

```bash
cd tests/wasm
npm ci
npx playwright install chromium
PJ_WASM_PACKAGE_DIR="$PWD/../../build-wasm/deploy" npm run test:deployment
```

The deployment test expects a package built with `--presets`: it also checks
every deployed preset file and that the page fetches `presets/index.json`.
Its response checks (`tests/wasm/support/deployment_contract.js`) also run
against Caddy with `docs/wasm/Caddyfile`, in `robot_deployment.spec.js`.
