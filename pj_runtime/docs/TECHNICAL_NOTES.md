# pj_runtime technical notes

Hard-won lessons behind code in this module, written as the current state of the code.

## Qt's browser network backend keeps every response body (Qt 6.12.0)

**What happens.** In the WebAssembly build, `QNetworkAccessManager` runs each request
through `emscripten_fetch()` with `EMSCRIPTEN_FETCH_LOAD_TO_MEMORY`.
`QNetworkReplyWasmImplPrivate::downloadSucceeded()` (and `downloadFailed()`) in
`qtbase/src/network/access/qnetworkreplywasmimpl.cpp` copy `fetch->data` into the reply
and then set `reply->m_fetch = nullptr` without calling `emscripten_fetch_close()`.
`QNetworkReplyWasmImpl::close()`, the only caller of `emscripten_fetch_close()`, then
finds no fetch, so the `emscripten_fetch_t` and its body buffer stay in the wasm heap
for the rest of the session. A download also peaks at three copies of its body:
`fetch->data`, the reply's buffer and the caller's `readAll()`.

**Measured.** Opening a preset with a 261 MB recording left the wasm heap at 1084 MiB;
a 96 MiB recording downloaded and then refused left 96 MiB more in use (`tests/wasm/presets.spec.js`,
"a preset recording downloads without passing through the wasm heap"), and a robot mesh kept its
whole body (`tests/wasm/scene3d_url_models.spec.js`, "a URL robot mesh leaves no copy in the wasm heap").

**What the code does.** `browserFetchToFile()` (`BrowserFetch.h`) bypasses Qt: the
page's `fetch()` writes each chunk into a MEMFS file, which MEMFS keeps as a JavaScript
array outside the wasm heap; the SHA-256 is one `crypto.subtle.digest` over that array once the body is complete.
`pj_app`'s `PresetController` streams preset recordings through it; every download that
wants the bytes (the other preset files, Scene3D's `UrlFetcher` for robot URDFs, meshes and
textures) goes through `HttpGet.h`'s `fetchBytesWithLimit()`, whose browser side is
`browserFetchToBytes()`: no hashing, a scratch file read back into a `QByteArray`.

**Re-test** on every Qt bump: once `downloadSucceeded()` closes the fetch, a
`QNetworkAccessManager` download stops leaking (it still peaks at the extra copies).

## The browser's resident payload pool is 64 MiB

`SessionManager` keeps 64 MiB of ingested object payloads resident in the browser, a
quarter of the desktop's 256 MiB: the wasm heap never shrinks and also holds everything
else. On a 261 MB recording playback and seeking cost the same as with 256 MiB. A single
payload larger than the pool is never admitted, so every read of it re-fetches it lazily
from its source.
