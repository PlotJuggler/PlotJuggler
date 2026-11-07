// SPDX-License-Identifier: MPL-2.0
// node --test packaging/web/worker.test.mjs

import assert from 'node:assert/strict';
import { test } from 'node:test';

import worker from './worker.mjs';

const RELEASE = 'releases/sha256-test';

function manifest() {
  const representations = (file, encoded) => ({
    identity: { path: file, bytes: 5, sha256: 'a'.repeat(64) },
    ...(encoded ? {
      br: { path: `${file}.br`, bytes: 3, sha256: 'b'.repeat(64), contentEncoding: 'br' },
      gzip: { path: `${file}.gz`, bytes: 4, sha256: 'c'.repeat(64), contentEncoding: 'gzip' },
    } : {}),
  });
  return {
    format: 'org.plotjuggler.wasm-deployment.v1',
    note: 'PlotJuggler — a non-ASCII byte count',
    entrypoint: 'index.html',
    files: [
      { path: 'index.html', contentType: 'text/html; charset=utf-8', cache: 'revalidate', representations: representations('index.html', true) },
      { path: 'assets/sha256-x/app.wasm', contentType: 'application/wasm', cache: 'immutable', representations: representations('assets/sha256-x/app.wasm', true) },
      { path: 'presets/index.json', contentType: 'application/json; charset=utf-8', cache: 'revalidate', representations: representations('presets/index.json', false) },
    ],
  };
}

function environment() {
  const objects = new Map(Object.entries({
    'manifest.json': JSON.stringify(manifest()),
    'index.html': 'index',
    'index.html.br': 'ibr',
    'index.html.gz': 'igzp',
    'assets/sha256-x/app.wasm': 'wasm!',
    'assets/sha256-x/app.wasm.br': 'wbr',
    'assets/sha256-x/app.wasm.gz': 'wgzp',
    'presets/index.json': '{"p"}',
    'sources/SOURCE-OFFER.txt': 'offer',
  }).map(([key, value]) => [`${RELEASE}/${key}`, new TextEncoder().encode(value)]));
  const forwarded = [];
  return {
    forwarded,
    env: {
      RELEASE,
      STATS_HOST: 'stats.example.test',
      SITE: {
        async get(key) {
          const bytes = objects.get(key);
          return bytes ? { body: new Blob([bytes]).stream(), size: bytes.length, text: async () => new TextDecoder().decode(bytes) } : null;
        },
      },
      TELEMETRY: {
        async fetch(request) {
          forwarded.push(request);
          return new Response('telemetry', { status: 202 });
        },
      },
    },
  };
}

async function get(env, path, init = {}, host = 'app.example.test', cf = undefined) {
  const request = new Request(`https://${host}${path}`, init);
  if (cf) {
    Object.defineProperty(request, 'cf', { value: cf });
  }
  return worker.fetch(request, env);
}

const ISOLATION = {
  'cross-origin-opener-policy': 'same-origin',
  'cross-origin-embedder-policy': 'require-corp',
  'cross-origin-resource-policy': 'same-origin',
  'x-content-type-options': 'nosniff',
};

function assertIsolated(response) {
  for (const [name, value] of Object.entries(ISOLATION)) {
    assert.equal(response.headers.get(name), value, name);
  }
}

test('the root serves the entrypoint, isolated and revalidated', async () => {
  const { env } = environment();
  const response = await get(env, '/');
  assert.equal(response.status, 200);
  assertIsolated(response);
  assert.equal(response.headers.get('content-type'), 'text/html; charset=utf-8');
  assert.equal(response.headers.get('cache-control'), 'no-cache');
  assert.equal(response.headers.get('vary'), 'Accept-Encoding');
  assert.equal(response.headers.get('content-encoding'), null);
  assert.equal(await response.text(), 'index');
});

test('a precompressed copy answers a client that accepts it', async () => {
  const { env } = environment();
  const br = await get(env, '/assets/sha256-x/app.wasm', { headers: { 'Accept-Encoding': 'gzip, br' } });
  assert.equal(br.headers.get('content-encoding'), 'br');
  assert.equal(br.headers.get('content-type'), 'application/wasm');
  assert.equal(br.headers.get('cache-control'), 'public, max-age=31536000, immutable');
  assert.equal(br.headers.get('etag'), `"sha256-${'b'.repeat(64)}"`);
  const gzip = await get(env, '/assets/sha256-x/app.wasm', { headers: { 'Accept-Encoding': 'gzip' } });
  assert.equal(gzip.headers.get('content-encoding'), 'gzip');
});

test("the client's own Accept-Encoding decides, not the one the edge rewrote", async () => {
  const { env } = environment();
  // The runtime replaces the header with what Cloudflare accepts; request.cf keeps what the client sent.
  const plain = await get(env, '/assets/sha256-x/app.wasm', { headers: { 'Accept-Encoding': 'gzip, br' } },
    undefined, { clientAcceptEncoding: 'identity' });
  assert.equal(plain.headers.get('content-encoding'), null);
  assert.equal(await plain.text(), 'wasm!');
});

test('a release without its manifest is a 500 that keeps the isolation headers', async () => {
  const { env } = environment();
  const response = await get({ ...env, RELEASE: 'releases/missing' }, '/');
  assert.equal(response.status, 500);
  assertIsolated(response);
});

test('a matching If-None-Match gets 304 and HEAD gets no body', async () => {
  const { env } = environment();
  const etag = `"sha256-${'a'.repeat(64)}"`;
  const notModified = await get(env, '/presets/index.json', { headers: { 'If-None-Match': etag } });
  assert.equal(notModified.status, 304);
  assertIsolated(notModified);
  const head = await get(env, '/presets/index.json', { method: 'HEAD' });
  assert.equal(head.status, 200);
  assert.equal(head.headers.get('etag'), etag);
  assert.equal(await head.text(), '');
});

test('paths outside the manifest are 404, other methods 405, both isolated', async () => {
  const { env } = environment();
  for (const path of ['/missing.js', '/assets/..%2findex.html', '/assets\\x', '/index.html.br']) {
    const response = await get(env, path);
    assert.equal(response.status, 404, path);
    assertIsolated(response);
  }
  const post = await get(env, '/index.html', { method: 'POST', body: 'x' });
  assert.equal(post.status, 405);
  assert.equal(post.headers.get('allow'), 'GET, HEAD');
});

test('the manifest itself is served, revalidated', async () => {
  const { env } = environment();
  const response = await get(env, '/manifest.json');
  assert.equal(response.status, 200);
  assert.equal(response.headers.get('cache-control'), 'no-cache');
  assert.equal(JSON.parse(await response.text()).entrypoint, 'index.html');
  const head = await get(env, '/manifest.json', { method: 'HEAD' });
  assert.equal(head.headers.get('content-length'), String(new TextEncoder().encode(JSON.stringify(manifest())).length));
});

test('the source offer is served beside the release', async () => {
  const { env } = environment();
  const offer = await get(env, '/sources/SOURCE-OFFER.txt');
  assert.equal(offer.status, 200);
  assert.equal(offer.headers.get('content-type'), 'text/plain; charset=utf-8');
  assertIsolated(offer);
  assert.equal(await offer.text(), 'offer');
  assert.equal((await get(env, '/sources/missing.tar.gz')).status, 404);
});

test('installed releases keep reporting: /telemetry goes to the telemetry worker as sent', async () => {
  const { env, forwarded } = environment();
  const response = await get(env, '/telemetry', {
    method: 'POST', body: '{"user_id":"u"}', headers: { 'CF-IPCountry': 'IT', 'Content-Type': 'application/json' },
  });
  assert.equal(response.status, 202);
  assert.equal(forwarded.length, 1);
  assert.equal(forwarded[0].method, 'POST');
  assert.equal(forwarded[0].headers.get('cf-ipcountry'), 'IT');
  assert.equal(await forwarded[0].text(), '{"user_id":"u"}');
});

test('the stats host is the telemetry worker, every path', async () => {
  const { env, forwarded } = environment();
  for (const path of ['/', '/api', '/analytics/daily-users?days=7']) {
    assert.equal((await get(env, path, {}, 'stats.example.test')).status, 202, path);
  }
  assert.deepEqual(forwarded.map(request => new URL(request.url).pathname), ['/', '/api', '/analytics/daily-users']);
});
