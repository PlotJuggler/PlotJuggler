// SPDX-License-Identifier: MPL-2.0

// The browser samples' files on PlotJuggler's asset host, served to the suites from a local mirror: the
// suites never reach the network (browser.js). `npm run fetch:assets` fills the mirror once, each file
// checked against its SHA-256 in fixtures/hosted_assets.json. PJ_WASM_ASSETS_MIRROR moves the mirror.
const fs = require('fs');
const http = require('http');
const path = require('path');

const MANIFEST = require('../fixtures/hosted_assets.json');
const MIRROR = path.resolve(
  process.env.PJ_WASM_ASSETS_MIRROR || path.join(__dirname, '..', '..', '..', 'build-wasm-assets'));
// The deployment's types (scripts/package_wasm.mjs) plus the hosted samples' own.
const HOSTED_CONTENT_TYPES = [['.glb', 'model/gltf-binary'], ['.jpg', 'image/jpeg'], ['.urdf', 'application/xml']];

// Why the mirror cannot serve the samples, or '' when every file is present at its manifest size
// (a stale file would stream a truncated body); the hash is fetch:assets' job.
function hostedAssetsMissing() {
  const missing = Object.entries(MANIFEST.files).filter(
    ([file, { bytes }]) => !fs.existsSync(path.join(MIRROR, file)) || fs.statSync(path.join(MIRROR, file)).size !== bytes);
  return missing.length === 0 ? '' : `${missing.length} hosted sample files are not mirrored in ${MIRROR}: `
    + 'run npm --prefix tests/wasm run fetch:assets';
}

// Serves the mirror on loopback, with the CORS header the host sends, and returns
// { origin, route(context, samples_origin), close() }. The mirror serves each hosted layout with the asset
// host rewritten to its own origin, and route() does the same to the preset index the context's pages
// download from `samples_origin` (the local sample server: route.fetch() runs outside the browser's network
// containment, so it may reach nothing else, nor follow a redirect), so the samples fetch every file from
// the mirror. Rewriting the small index and layouts, not fulfilling the files themselves: Chromium does not
// deliver a fulfilled body as large as a recording.
async function startHostedAssetMirror() {
  const { CONTENT_TYPES } = await import('../../../scripts/package_wasm.mjs');
  const contentTypes = new Map([...CONTENT_TYPES, ...HOSTED_CONTENT_TYPES]);
  const server = http.createServer((request, response) => {
    const file = decodeURIComponent(new URL(request.url, 'http://mirror').pathname.slice(1));
    if (!Object.hasOwn(MANIFEST.files, file)) {
      response.writeHead(404, { 'Access-Control-Allow-Origin': '*' }).end();
      return;
    }
    const headers = {
      'Access-Control-Allow-Origin': '*',
      'Content-Type': contentTypes.get(path.extname(file)) || contentTypes.get(''),
    };
    if (file.endsWith('.pj4.xml')) {
      const layout = fs.readFileSync(path.join(MIRROR, file), 'utf8').replaceAll(MANIFEST.origin, origin);
      response.writeHead(200, headers).end(layout);
      return;
    }
    response.writeHead(200, { ...headers, 'Content-Length': MANIFEST.files[file].bytes });
    fs.createReadStream(path.join(MIRROR, file)).pipe(response);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = `http://127.0.0.1:${server.address().port}`;
  return {
    origin,
    route: (context, samplesOrigin) => context.route(`${samplesOrigin}/presets/index.json`, async (route) => {
      const response = await route.fetch({ maxRedirects: 0 });
      const body = (await response.text()).replaceAll(MANIFEST.origin, origin);
      return route.fulfill({ response, body });
    }),
    close: () => new Promise(resolve => server.close(resolve)),
  };
}

// Downloads every manifest file the mirror lacks or holds with other bytes.
async function fetchHostedAssets() {
  const { sha256 } = await import('../../../scripts/wasm_manifest.mjs');
  for (const [file, { sha256: expected }] of Object.entries(MANIFEST.files)) {
    const target = path.join(MIRROR, file);
    if (fs.existsSync(target) && sha256(fs.readFileSync(target)) === expected) {
      continue;
    }
    const response = await fetch(`${MANIFEST.origin}/${file}`);
    if (!response.ok) {
      throw new Error(`${file}: HTTP ${response.status}`);
    }
    const bytes = Buffer.from(await response.arrayBuffer());
    if (sha256(bytes) !== expected) {
      throw new Error(`${file}: SHA-256 ${sha256(bytes)}, expected ${expected}`);
    }
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.writeFileSync(target, bytes);
    console.log(`mirrored ${file}`);
  }
}

if (require.main === module) {
  fetchHostedAssets().catch((error) => {
    console.error(error.message);
    process.exit(1);
  });
}

module.exports = { HOSTED_MIRROR: MIRROR, hostedAssetsMissing, startHostedAssetMirror };
