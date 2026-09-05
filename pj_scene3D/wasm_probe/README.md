# Scene3D WebAssembly capability probe

This is a retained feasibility test, not a product renderer. It is built only
with Emscripten and only when `PJ_WASM_SCENE3D_CAPABILITY_PROBE=ON`; the option
defaults to off and does not enable `PJ_WASM_WITH_SCENE3D`.

Using the pinned Qt/Emscripten environment:

```bash
source "${EMSDK}/emsdk_env.sh"   # EMSDK points at your Emscripten SDK checkout
cmake -S . -B build-wasm -DPJ_WASM_SCENE3D_CAPABILITY_PROBE=ON
cmake --build build-wasm --target pj_scene3d_wasm_capability_probe -j4
emrun --no-browser --serve-after-exit --port 6931 \
  build-wasm/pj_app/scene3d_capability_probe.html
```

In another shell, install Chromium and run the probe:

```bash
cd tests/wasm
npx playwright install --with-deps chromium
npm run test:scene3d-capability
```

The server must supply the COOP/COEP headers required by the threaded Qt kit;
the test also asserts `crossOriginIsolated` and `SharedArrayBuffer` directly.
Every run attaches its complete JSON report. The left half of each widget is
produced by a vertex-stage `texelFetch` from a 3D texture, matching the product
voxel renderer's capability dependency; the right half retains the
fragment-stage sample from an RGBA16F target.

The six `.qsb` files contain GLSL ES 300 payloads generated with the host Qt
`qsb` from the pinned Qt in `versions.env`. Set `QSB` to that binary — CMake
resolves the same tool as `$<TARGET_FILE:Qt6::qsb>`:

```bash
"${QSB}" --glsl "300 es" \
  -o shaders/point.vert.qsb shaders/point.vert
```

Repeat that command for each shader after editing it. CMake rebakes every pack
and compares its hash, so stale committed binaries fail configuration.
