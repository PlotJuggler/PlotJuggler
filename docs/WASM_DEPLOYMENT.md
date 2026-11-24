# PlotJuggler 4 WebAssembly deployment

PlotJuggler's browser build is a static, threaded Qt/WebAssembly application.
It must be served over HTTPS (or localhost during development) with
cross-origin isolation enabled. Opening `index.html` directly from disk or
using an ordinary `python -m http.server` is not a supported threaded setup:
the packaged entrypoint then shows why on the page ("PlotJuggler cannot start:
this page was not served over HTTPS." or "…is not cross-origin isolated.", with
what the server must do) instead of loading the runtime. When the server does send
the headers and the page still is not isolated, the browser itself cannot isolate
it (an Android WebView, as in DuckDuckGo), and the page says to open another one. The module is built with
WebAssembly SIMD, so it needs Chrome or Edge 91, Firefox 89 or Safari 16.4 (iOS 16.4) or later; an older engine gets
"PlotJuggler cannot start in this browser: it does not support WebAssembly SIMD." To serve the app from
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

The whole module, including fetched dependencies, compiles and links with
`-fexceptions` (root `CMakeLists.txt`): Emscripten drops the catch blocks of any
translation unit built without it, so do not set it per target. These are
Emscripten's JavaScript-based exceptions; `-fwasm-exceptions` would need a Qt
kit built with `wasm_exceptions`, which the official kit is not.

A public deployment also publishes the source offer
([licence and source offer](#licence-of-the-browser-bundle-and-the-source-offer)):
`node scripts/wasm_source_offer.mjs --links --output build-wasm/deploy/sources`.

A version tag builds this package in WASM CI too, stamped with the tag's version and with its source offer, and
the `wasm-release` job attaches it to the tag's GitHub Release as `PlotJuggler4-web-bundle.zip` once the Chromium
suite has passed. The name carries no version, so `releases/latest/download/PlotJuggler4-web-bundle.zip` reaches the
copy of the release GitHub marks as latest, once that job has finished. A manual run of WASM CI on a branch builds
the same archive as its `wasm-release` artifact, without the Release upload. Unzipped, it deploys to any host that
meets the [Required HTTP contract](#required-http-contract); `app.plotjuggler.io` is published by hand
([packaging/web/README.md](../packaging/web/README.md)).

`--presets` is optional: it deploys a preset tree (the repository's samples
here) under `presets/`; see [Presets](#presets). The `app.plotjuggler.io`
package is built without it ([`packaging/web/README.md`](../packaging/web/README.md)).

### Built-in plugins

The browser cannot `dlopen` extensions, so a fixed set of official plugins is
compiled from a pinned `pj-official-plugins` commit (`cmake/PjWasmDependencies.cmake`)
and registered statically by `pj_app/src/BuiltinPlugins.cpp`. Each plugin describes
its own WebAssembly build in a `wasm.cmake` next to its sources (the contract with this
build is in that repository's `cmake/PjWasmPlugin.cmake`); `cmake/PjWasmOfficialPlugins.cmake`
lists the plugins, creates one option per entry, all `ON` by default, and generates
their registration (the Arrow family builds Apache Arrow C++ through `PjWasmArrowDependencies`):

| Option | Plugin |
|---|---|
| `PJ_WASM_WITH_CSV` | `data_load_csv` |
| `PJ_WASM_WITH_MCAP` | `data_load_mcap` |
| `PJ_WASM_WITH_ULOG` | `data_load_ulog` (PX4 ULog) |
| `PJ_WASM_WITH_PARQUET` | `data_load_parquet` (named non-UTC time zones stay unadjusted: the browser has no tz database) |
| `PJ_WASM_WITH_CANDUMP` | `data_load_candump` (SocketCAN logs) |
| `PJ_WASM_WITH_BLF` | `data_load_blf` (Vector BLF) |
| `PJ_WASM_WITH_MF4` | `data_load_mf4` (ASAM MDF4) |
| `PJ_WASM_WITH_3D_LOADER` | `data_load_3d` (PCD/PLY) |
| `PJ_WASM_WITH_DUMMY_STREAM` | `data_stream_dummy` |
| `PJ_WASM_WITH_PJ_BRIDGE` | `data_stream_pj_bridge` (over the browser's WebSocket) |
| `PJ_WASM_WITH_FOXGLOVE_BRIDGE` | `data_stream_foxglove_bridge` (over the browser's WebSocket; a robot's `ws://` bridge needs TLS in front of it on an `https://` page) |
| `PJ_WASM_WITH_ROS` | `parser_ros` |
| `PJ_WASM_WITH_PROTOBUF` | `parser_protobuf` |
| `PJ_WASM_WITH_JSON` | `parser_json` |
| `PJ_WASM_WITH_DATA_TAMER` | `parser_data_tamer` |
| `PJ_WASM_WITH_INFLUX` | `parser_influx` (InfluxDB line protocol) |
| `PJ_WASM_WITH_NAVMAP` | `parser_navmap` |
| `PJ_WASM_WITH_ARROW_PARSER` | `parser_arrow` (Arrow IPC) |
| `PJ_WASM_WITH_TRANSFORM_EDITOR` | `toolbox_transform_editor` |
| `PJ_WASM_WITH_QUATERNION` | `toolbox_quaternion` |
| `PJ_WASM_WITH_FFT` | `toolbox_fft` |
| `PJ_WASM_WITH_DATA_EXPORTER` | `toolbox_data_exporter` (the exported files download from the browser) |
| `PJ_WASM_WITH_ANOMALY_DETECTOR` | `toolbox_anomaly_detector`, the Marker Editor (the host runs its rules) |

Visualization families are on by default too: `PJ_WASM_WITH_SCENE2D` (images,
depth, annotations, and with `PJ_WASM_WITH_VIDEO` software H.264/HEVC/AV1 video
from a statically linked LGPL FFmpeg) and `PJ_WASM_WITH_SCENE3D`.

Plugins that need raw sockets, child processes or native middleware (UDP, ZMQ,
MQTT, LSL, ROS 2, the AI assistant) cannot run in a browser. The PlotJuggler
Bridge needs none of them: it connects with the browser's own WebSocket, so a page
served over HTTPS reaches only `wss://` bridges (mixed content), and the browser
reports a failed connection without its cause. Its stream presets are described
in [`WASM_PRESETS.md`](./WASM_PRESETS.md#streaming-sources). Adding another
plugin is described in the "WebAssembly build" section of the
[pj-official-plugins README](https://github.com/PlotJuggler/pj-official-plugins#webassembly-build):
a `wasm.cmake` in its directory there, one line in `PJ_WASM_OFFICIAL_PLUGIN_DIRS`
here, and a pin that contains it.

The pin is moved by `.github/workflows/wasm-plugins-pin.yml`, daily at 05:17 UTC, one pull
request at a time. `scripts/bump_wasm_plugins.sh` resolves the head of `pj-official-plugins`
`main` to a full commit sha, takes the sha-256 of its codeload archive and rewrites the two pin
lines; the PR from `bot/wasm-plugins-pin` lists the plugin commits it brings. A head that is
not ahead of the pin is never proposed, and while a PR from that branch is open the workflow
does nothing. A PR made with `GITHUB_TOKEN` gets no checks, so the workflow dispatches WASM CI
on the branch and links the run in a PR comment: that run must be green before the PR is
merged by hand. Closing the PR without merging lets the next run open one for the plugins head
of that day. `gh workflow run wasm-plugins-pin.yml -f ref=<branch, tag or full sha>` pins
another ref (the PR body says when it is not ahead of the pin); `scripts/bump_wasm_plugins.sh
[<ref>]` edits the pin in a local tree. To build against a plugins checkout without changing
the pin, configure with `-DFETCHCONTENT_SOURCE_DIR_PJ_OFFICIAL_PLUGINS=<path>`.

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
(on the helpers of `cmake/PjNotices.cmake`, which also writes the desktop
notices: [packaging/README.md](../packaging/README.md#licences)) writes `THIRD_PARTY_NOTICES.txt` beside `plotjuggler4.js` at configure time: one
section per library with its SPDX licence expression and its licence text,
copied from its source (Qt's modules, plugins and bundled third-party code from
the kit's SBOM, `sbom/*.spdx.json`, followed by the text of every standard
licence they name, one file each in `cmake/notices/<SPDX id>.txt`; the
configure fails, naming the id, when one is missing), and CPython's notices when
Python is on.
`package_wasm.mjs` refuses a build without the file. `npm run test:package`
(`tests/wasm/wasm_notices.test.mjs`) fails when a `FetchContent_Declare` of the
WASM build has no `pj_wasm_notice()` entry. What that test cannot see is kept
by hand in the same file: a Qt module or plugin added to the link
(`_PJ_QT_NOTICE_PACKAGES`), a vendored directory or font, third-party code
bundled inside a dependency (assimp's `contrib/`, the plugins' `contrib/`,
rosx_introspection's `contrib/`), and the Emscripten runtime. In the browser build the About
dialog states the bundle's GPL-3.0 licence, and its "Third-party licences" and
"Source code" links open `THIRD_PARTY_NOTICES.txt` and `sources/SOURCE-OFFER.txt`
by those relative names, so the package serves a copy at the deployment root
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
archives only PlotJuggler 4 and gives every other part its public address: the
URL and SHA-256 of a hashed pin, the commit and
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
build too; `-DPJ_WASM_WITH_PYTHON=OFF` builds the
app without it, and a recipe in Python then fails to restore with "python is
not available in this build". WASM CI's functional suite runs
`tests/wasm/python.spec.js` (the lane's limits, per-call cost and main-thread
gaps) and `tests/wasm/python_layout.spec.js` (a layout's Python recipes
restored into a 3D scene, a broken one showing its compile error) in Chromium
and Firefox. Python filters stay unavailable in the browser; what else remains
is in [`derived_objects/ARCHITECTURE.md`](./derived_objects/ARCHITECTURE.md) "Scripting languages".

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

- **Size budget.** At startup it adds the interpreter linked into the module,
  about 1.2 MB of Brotli download (measured 2026-11-15: production package
  15.19 MB with Python). CI fails the production package's Brotli download above
  the budget `wasm-ci.yml` passes to `scripts/wasm_size_budget.mjs` (the sample
  `presets/` are fetched on demand and not counted). The budget is absolute
  because every configuration carries Python: a delta would need a third build.
  The stdlib zip (0.67 MB Brotli, stored uncompressed so the
  package's Brotli compresses it) is not part of it: it is a separate asset,
  `python_stdlib.zip` in the content-addressed bundle beside the module, which
  the first Python use downloads (a synchronous request on the Python worker,
  `python_sandbox.cpp`) to `/lib/python314.zip` before the interpreter boots.
  The budget leaves it out. A failed download (the network, an HTTP error,
  a body that is not a zip; a 404 means the app was redeployed since the page
  loaded, so it says to reload) fails that use with the reason and is not
  kept by the script: uses within 10 s of the failure get the same error
  without a new request (a playback re-runs scripts every frame), the first
  use after downloads again, the same recipe included; a 404 is answered
  without a request for the rest of the session. A validate meanwhile reports
  the script unchecked, not invalid (`tests/wasm/python.spec.js`).
- **The stdlib zip** holds only the modules that importing the roots in
  `stdlib_zip.py` loads. A module the stdlib imports
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
browser identities; they are not persisted across refreshes. A file opened from
the picker is read from disk on demand rather than copied, so its size is not
bounded by the tab's memory; it must stay unchanged on disk while it is open
(a changed file reports that its data may be incomplete, see
[`pj_app/docs/ARCHITECTURE.md`](../pj_app/docs/ARCHITECTURE.md#browser-webassembly-boundary)). Ordinary
`QSettings` callers are likewise redirected to page-lifetime storage. The only
durable browser state is the typed preference allowlist and a bounded list of
source-free generic layout recipes owned by `BrowserPersistence`. Source-bound
layouts are downloaded with logical references and require explicit file
reselection when replayed. Paths, upload identities, plugin configuration, and
credentials are intentionally excluded from browser-local persistence.

A source's plugin may also read files picked in its own dialog (a CAN
dictionary, say). "Save Layout with Sources" saves each such *side file* like
the data file: every string of the plugin configuration that is a staged
file other than the data file is replaced by a logical name relative to the
data file's directory, and listed as
`<sideFile filename="…" content_sha256="…"/>` under the source's `<fileInfo>`.
The name is the file's basename, or the first free `pj4-side/<n>/<basename>`
when that is already the data file's name or a string of the configuration.
One file staged twice (same name and fingerprint) is one side file. The data
file's staging folder, if the configuration names it, is saved as an empty
string. Any other staged path still refuses the save. On replay the page asks
for every side file after the data files (same name rule as a data file; the
fingerprint tells same-named files apart), and a layout opened by URL
downloads each one resolved against its data file's URL, checked against its
fingerprint like a data file. Every configuration string equal to a
`filename` is then rewritten to the staged copy before the import. On the
desktop the same rewrite points each `filename` at that name in the loaded
data file's folder. `<sideFile>` is optional.

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
- **Deep link:** `?preset=<id>` opens that preset directly: the dialog shows
  only its progress and Cancel, never the list, which appears only if the load
  fails or is cancelled. An id the index does not list, the `preset` key given twice, or
  a deep link on a page without a usable index reports an error and loads
  nothing. Other query keys are ignored.

The repository ships six samples, listed by `resources/wasm-presets/index.json`
and deployed with `--presets` (a car's gyro through a built-in filter; an ANYmal
walking a forest trail; a car from PandaSet with its six cameras, lidar and 3D
boxes; an office robot from LILocBench with its camera, depth cloud and laser
scans on the floor map; a racing quadrotor from UZH-FPV with its fisheye and event
cameras and its racing line; Pollen Robotics' Microduck biped walking and falling,
with a formula that labels what it is doing as text). The welcome dialog lists the
first three; the office robot, the racing drone and the biped are unlisted and open
from their links, `?preset=lilocbench`, `?preset=uzhfpv` and `?preset=microduck`,
which the website's examples page uses. Their layouts, thumbnails, recordings and models load from
`https://assets.plotjuggler.io`. Preset files are served `no-cache`. `app.plotjuggler.io`
does not package the index but serves an uploaded copy
([`packaging/web/README.md`](../packaging/web/README.md#sample-index)). How to author a preset,
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

The deployment test checks that the page requests `presets/index.json`; on a
package built with `--presets` it also checks every deployed preset file, and
with `PJ_WASM_DEPLOYMENT_URL` (a published `app.plotjuggler.io` Worker) the
Worker's index route.
Its response checks (`tests/wasm/support/deployment_contract.js`) also run
against Caddy with `docs/wasm/Caddyfile`, in `robot_deployment.spec.js`.

## Deterministic browser fixtures

A browser test fixture must reproduce both the bytes and the UI's product state, not only the data. The Timeline
prefers scalar and object bounds, and extra TF samples do not widen it, so a Scene3D seek fixture needs at least two
object-bound samples.
