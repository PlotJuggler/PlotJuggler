# PlotJuggler 4 WebAssembly deployment

PlotJuggler's browser build is a static, threaded Qt/WebAssembly application.
It must be served over HTTPS (or localhost during development) with
cross-origin isolation enabled. Opening `index.html` directly from disk or
using an ordinary `python -m http.server` is not a supported threaded setup.

Qt's deployment guidance describes the generated HTML, JavaScript loader, and
Wasm module, recommends gzip or Brotli compression, and documents the headers
needed by a multithreaded build:
[Qt for WebAssembly](https://doc.qt.io/qt-6/wasm.html). Emscripten separately
requires `application/wasm` for streaming compilation and COOP/COEP for
Pthreads:
[WebAssembly server setup](https://emscripten.org/docs/compiling/WebAssembly.html#web-server-setup),
[Pthreads support](https://emscripten.org/docs/porting/pthreads.html).

## Build and package

The exact release toolchain is pinned in `versions.env`. A local equivalent of
the release workflow is:

```bash
source /path/to/emsdk/emsdk_env.sh
cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/Qt/<PJ_QT_VERSION>/wasm_multithread/lib/cmake/Qt6/qt.toolchain.cmake \
  -DQT_HOST_PATH=/path/to/Qt/<PJ_QT_VERSION>/gcc_64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DPJ_BUILD_TESTS=OFF -DPJ_BUILD_DEMOS=OFF \
  -DPJ_WASM_WITH_SCENE2D=ON -DPJ_WASM_WITH_SCENE3D=OFF
cmake --build build-wasm --target pj_app --parallel 4

node scripts/package_wasm.mjs \
  --source build-wasm/pj_app \
  --output build-wasm/deploy
```

The packaging command refuses missing, invalid, or single-threaded build
outputs. To prevent a mistyped `--output` from deleting unrelated data, it
only replaces an absent directory, an empty directory, or a prior package with
a valid PlotJuggler deployment manifest. It emits a stable entrypoint and
content-addressed assets:

```text
build-wasm/deploy/
├── index.html
├── manifest.json
├── _headers
└── assets/sha256-<bundle-id>/
    ├── plotjuggler4.js[.br|.gz]
    ├── plotjuggler4.wasm[.br|.gz]
    ├── qtloader.js[.br|.gz]
    └── qtlogo.svg[.br|.gz]
```

`manifest.json` records every identity/Brotli/gzip path, byte size, SHA-256,
MIME type, cache class, and compression runtime/parameters. Gzip metadata is
normalized, Brotli uses a fixed quality/window, and release CI pins Node, so
identical inputs produce byte-identical packages. The generated `_headers`
file's path, size, and hash are covered separately as deployment configuration;
the file configures isolation and cache policy on hosts that
support the Netlify/Cloudflare Pages format; it does not make a host select
precompressed variants automatically.

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
remote server; configure that data origin explicitly.

## Browser-local state

Uploaded files are staged in the page's temporary filesystem and receive opaque
browser identities; they are not persisted across refreshes. Ordinary
`QSettings` callers are likewise redirected to page-lifetime storage. The only
durable browser state is the typed preference allowlist and a bounded list of
source-free generic layout recipes owned by `BrowserPersistence`. Source-bound
layouts are downloaded with logical references and require explicit file
reselection when replayed. Paths, upload identities, plugin configuration, and
credentials are intentionally excluded from browser-local persistence.

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
