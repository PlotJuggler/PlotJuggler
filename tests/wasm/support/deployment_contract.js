// SPDX-License-Identifier: MPL-2.0
// The deployment package's HTTP contract (docs/WASM_DEPLOYMENT.md, "Required HTTP contract"),
// checked against whichever server hosts the package: scripts/serve_wasm.mjs in
// deployment.spec.js, Caddy with docs/wasm/Caddyfile in robot_deployment.spec.js.
const { createHash } = require('node:crypto');
const http = require('node:http');
const https = require('node:https');
const { expect } = require('@playwright/test');

const CACHE_CONTROL = {
  revalidate: 'no-cache',
  immutable: 'public, max-age=31536000, immutable',
};
// The Accept-Encoding that selects each representation.
const ACCEPT_ENCODING = { br: 'br, identity;q=0', gzip: 'gzip, identity;q=0', identity: 'identity' };

// One request whose body is hashed as received, still encoded. Other options go to
// http(s).request, such as a test CA (`ca`) and `lookup` for an https origin.
function rawRequest(url, { method = 'GET', headers = {}, collectBody = false, ...options } = {}) {
  const transport = new URL(url).protocol === 'https:' ? https : http;
  return new Promise((resolve, reject) => {
    const request = transport.request(url, { method, headers, ...options }, response => {
      const hash = createHash('sha256');
      const chunks = [];
      let bytes = 0;
      response.on('data', chunk => {
        hash.update(chunk);
        bytes += chunk.length;
        if (collectBody) {
          chunks.push(chunk);
        }
      });
      response.on('end', () => resolve({
        status: response.statusCode,
        headers: response.headers,
        bytes,
        sha256: hash.digest('hex'),
        body: collectBody ? Buffer.concat(chunks).toString('utf8') : undefined,
      }));
    });
    request.on('error', reject);
    request.end();
  });
}

function expectIsolationHeaders(headers) {
  expect(headers['cross-origin-opener-policy']).toBe('same-origin');
  expect(headers['cross-origin-embedder-policy']).toBe('require-corp');
  expect(headers['cross-origin-resource-policy']).toBe('same-origin');
  expect(headers['x-content-type-options']).toBe('nosniff');
}

// `/` answers the revalidated entrypoint, which loads the content-addressed bundle.
async function expectEntrypoint(baseUrl, manifest, options = {}) {
  const entrypoint = await rawRequest(`${baseUrl}/`, {
    ...options,
    headers: { 'Accept-Encoding': 'identity' },
    collectBody: true,
  });
  expect(entrypoint.status).toBe(200);
  expect(entrypoint.headers['content-type']).toBe('text/html; charset=utf-8');
  expect(entrypoint.headers['cache-control']).toBe('no-cache');
  expect(entrypoint.headers['content-encoding']).toBeUndefined();
  expectIsolationHeaders(entrypoint.headers);
  expect(entrypoint.body).toContain(`src="assets/${manifest.bundleId}/plotjuggler4.js"`);
  expect(entrypoint.body).toContain(`src="assets/${manifest.bundleId}/qtloader.js"`);
  expect(entrypoint.body).toContain(`const WASM_URL = 'assets/${manifest.bundleId}/plotjuggler4.wasm';`);
}

// Requests every representation a manifest `file` entry declares and checks its exact bytes,
// MIME, encoding, cache class and isolation headers. A file with only an identity
// representation must stay identity although the client accepts br and gzip: the server never
// compresses it on its own. Returns the responses by representation name.
async function expectServedAsDeclared(baseUrl, file, options = {}) {
  const encoded = Object.keys(file.representations).length > 1;
  const responses = {};
  for (const [name, expected] of Object.entries(file.representations)) {
    const response = await rawRequest(`${baseUrl}/${file.path}`, {
      ...options,
      headers: { 'Accept-Encoding': encoded ? ACCEPT_ENCODING[name] : 'br, gzip' },
    });
    const what = `${file.path} (${name})`;
    expect(response.status, what).toBe(200);
    expect(response.headers['content-type'], what).toBe(file.contentType);
    expect(response.headers['content-encoding'], what).toBe(expected.contentEncoding);
    expect(response.headers['cache-control'], what).toBe(CACHE_CONTROL[file.cache]);
    if (encoded) {
      expect(response.headers.vary, what).toBe('Accept-Encoding');
    }
    expect(response.bytes, what).toBe(expected.bytes);
    expect(response.sha256, what).toBe(expected.sha256);
    expectIsolationHeaders(response.headers);
    responses[name] = response;
  }
  return responses;
}

// The deployment configuration and the encoded siblings are not addressable resources: a client
// reaches an encoding only through its logical URL. No error is cacheable: a bundle URL that 404s
// while a package is still being copied must work once the copy completes, although bundle files
// are immutable.
async function expectUndeclaredHidden(baseUrl, manifest, options = {}) {
  const wasm = manifest.files.find(file => file.path.endsWith('/plotjuggler4.wasm'));
  const refusals = {
    configuration: await rawRequest(`${baseUrl}/_headers`, options),
    encodedSibling: await rawRequest(`${baseUrl}/${wasm.representations.br.path}`, options),
    missingBundleFile: await rawRequest(`${baseUrl}/assets/${manifest.bundleId}/missing.js`, options),
  };
  for (const [name, response] of Object.entries(refusals)) {
    expect(response.status, name).toBe(404);
    expect(response.headers['cache-control'], name).toBe('no-store');
    expectIsolationHeaders(response.headers);
  }
}

module.exports = {
  expectEntrypoint,
  expectIsolationHeaders,
  expectServedAsDeclared,
  expectUndeclaredHidden,
  rawRequest,
};
