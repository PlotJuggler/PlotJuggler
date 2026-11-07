// SPDX-License-Identifier: MPL-2.0
const { createHash } = require('node:crypto');
const { readFile } = require('node:fs/promises');
const path = require('node:path');
const { test, expect, devices } = require('@playwright/test');
const {
  expectEntrypoint,
  expectIsolationHeaders,
  expectServedAsDeclared,
  expectUndeclaredHidden,
  rawRequest,
} = require('./support/deployment_contract');

const packageRoot = path.resolve(
  process.env.PJ_WASM_PACKAGE_DIR || path.join(__dirname, '..', '..', 'build-wasm', 'deploy'),
);
let server;
let baseUrl;
let manifest;

test.beforeAll(async () => {
  manifest = JSON.parse(await readFile(path.join(packageRoot, 'manifest.json'), 'utf8'));
  // A published copy of the same package (packaging/web) instead of the reference server.
  if (process.env.PJ_WASM_DEPLOYMENT_URL) {
    baseUrl = process.env.PJ_WASM_DEPLOYMENT_URL.replace(/\/+$/, '');
    return;
  }
  const { listenWasmServer } = await import('../../scripts/serve_wasm.mjs');
  server = await listenWasmServer(packageRoot);
  const address = server.address();
  baseUrl = `http://127.0.0.1:${address.port}`;
});

test.afterAll(async () => {
  if (server) {
    await new Promise((resolve, reject) => server.close(error => (error ? reject(error) : resolve())));
  }
});

test('deployment server negotiates exact compressed bytes, MIME, isolation, and cache policy', async () => {
  expect(manifest.format).toBe('org.plotjuggler.wasm-deployment.v1');
  expect(manifest.bundleId).toMatch(/^sha256-[0-9a-f]{32}$/);
  expect(manifest.packagingRuntime).toEqual({
    node: process.version,
    zlib: process.versions.zlib,
    brotli: { quality: 9, window: 22 },
    gzip: { level: 9, mtime: 0, os: 255 },
  });
  const headersBytes = await readFile(path.join(packageRoot, '_headers'));
  const headersMetadata = manifest.deploymentConfiguration.headersTemplate;
  expect(headersBytes.length).toBe(headersMetadata.bytes);
  expect(createHash('sha256').update(headersBytes).digest('hex')).toBe(headersMetadata.sha256);
  const headersTemplate = headersBytes.toString('utf8');
  expect(headersTemplate).toContain('/\n  Cache-Control: no-cache');
  expect(headersTemplate).toContain(
    `/assets/${manifest.bundleId}/*\n  Cache-Control: public, max-age=31536000, immutable`,
  );

  await expectEntrypoint(baseUrl, manifest);

  const wasm = manifest.files.find(file => file.path.endsWith('/plotjuggler4.wasm'));
  expect(wasm).toBeDefined();
  expect(Object.keys(wasm.representations).sort()).toEqual(['br', 'gzip', 'identity']);
  // The contract checks follow the manifest; these pin what the manifest itself must declare.
  expect(wasm.cache).toBe('immutable');
  expect(wasm.representations.br.contentEncoding).toBe('br');
  expect(wasm.representations.gzip.contentEncoding).toBe('gzip');
  const responses = await expectServedAsDeclared(baseUrl, wasm);
  for (const [name, response] of Object.entries(responses)) {
    expect(response.headers.etag).toBe(`"sha256-${wasm.representations[name].sha256}"`);
  }

  const cached = await rawRequest(`${baseUrl}/${wasm.path}`, {
    headers: {
      'Accept-Encoding': 'br, identity;q=0',
      'If-None-Match': `"sha256-${wasm.representations.br.sha256}"`,
    },
  });
  expect(cached.status).toBe(304);
  expect(cached.bytes).toBe(0);
  expect(cached.headers['content-encoding']).toBe('br');
  expectIsolationHeaders(cached.headers);

  const head = await rawRequest(`${baseUrl}/${wasm.path}`, {
    method: 'HEAD',
    headers: { 'Accept-Encoding': 'gzip, identity;q=0' },
  });
  expect(head.status).toBe(200);
  expect(head.bytes).toBe(0);
  expect(head.headers['content-length']).toBe(String(wasm.representations.gzip.bytes));
  expect(head.headers['content-encoding']).toBe('gzip');
  expectIsolationHeaders(head.headers);

  await expectUndeclaredHidden(baseUrl, manifest);
});

test('deployed presets are revalidated and negotiated like the bundle, with every indexed file present', async () => {
  const presets = manifest.files.filter(file => file.path.startsWith('presets/'));
  const declared = new Set(presets.map(file => file.path));
  expect(declared.has('presets/index.json')).toBe(true);
  const index = JSON.parse(await readFile(path.join(packageRoot, 'presets', 'index.json'), 'utf8'));
  expect(index.presets.length).toBeGreaterThan(0);
  for (const entry of index.presets) {
    for (const reference of [entry.layout, entry.thumbnail].filter(Boolean)) {
      expect(declared.has(`presets/${reference}`), reference).toBe(true);
    }
  }
  expect((await readFile(path.join(packageRoot, '_headers'), 'utf8'))).toContain('/presets/*\n  Cache-Control: no-cache');

  // Formats compressed inside gain nothing from .br/.gz; nor does a file below the packager's 1 KiB floor.
  const compressedInside = /\.(mcap|png|glb)$/i;
  for (const file of presets) {
    expect(file.cache, file.path).toBe('revalidate');
    const expected = compressedInside.test(file.path) || file.representations.identity.bytes < 1024
      ? ['identity'] : ['br', 'gzip', 'identity'];
    expect(Object.keys(file.representations).sort(), file.path).toEqual(expected);
    await expectServedAsDeclared(baseUrl, file);
  }
  expect(presets.some(file => file.representations.br), 'some sample is precompressed').toBe(true);
});

test('cold Chromium boot runs the threaded app from the content-addressed package', async ({ page }) => {
  const consoleMessages = [];
  const pageErrors = [];
  const responses = new Map();
  page.on('console', message => consoleMessages.push(message.text()));
  page.on('pageerror', error => pageErrors.push(error.stack || error.message));
  page.on('response', response => responses.set(response.url(), response.headers()));

  await page.goto(`${baseUrl}/`, { waitUntil: 'domcontentloaded' });
  const startup = await page.waitForFunction(() => {
    const state = document.body?.dataset.state;
    if (state === 'failed') {
      return { error: document.querySelector('#status')?.textContent || '' };
    }
    const screen = document.querySelector('#screen');
    if (state === 'ready' && screen && getComputedStyle(screen).display !== 'none') {
      return { ready: true };
    }
    return false;
  }, undefined, { timeout: 90_000 });
  const startupState = await startup.jsonValue();
  expect(startupState.error, `Qt startup failed: ${startupState.error}`).toBeUndefined();
  await expect.poll(
    () => consoleMessages.some(message => message.includes('Scanning 0 plugin folder(s)')),
  ).toBe(true);
  await expect.poll(() => page.evaluate(() => crossOriginIsolated)).toBe(true);
  expect(await page.evaluate(() => isSecureContext)).toBe(true);
  expect(await page.evaluate(() => typeof SharedArrayBuffer)).toBe('function');

  const assetBase = `${baseUrl}/assets/${manifest.bundleId}`;
  const wasmHeaders = responses.get(`${assetBase}/plotjuggler4.wasm`);
  expect(wasmHeaders).toBeDefined();
  expect(wasmHeaders['content-type']).toBe('application/wasm');
  expect(wasmHeaders['content-encoding']).toBe('br');
  expect(wasmHeaders['cache-control']).toBe('public, max-age=31536000, immutable');
  expect(responses.has(`${assetBase}/plotjuggler4.js`)).toBe(true);
  expect(responses.has(`${assetBase}/qtloader.js`)).toBe(true);
  // The production build discovers the samples deployed beside the page.
  await expect.poll(() => responses.get(`${baseUrl}/presets/index.json`)?.['cache-control']).toBe('no-cache');
  expect(pageErrors).toEqual([]);
});

test('a phone is warned before the download and starts only on Continue anyway', async ({ browser }) => {
  const context = await browser.newContext({ ...devices['Pixel 7'] });
  const page = await context.newPage();
  const pageErrors = [];
  const wasmRequests = [];
  page.on('pageerror', error => pageErrors.push(error.stack || error.message));
  page.on('request', request => {
    if (request.url().endsWith('.wasm')) {
      wasmRequests.push(request.url());
    }
  });
  try {
    await page.goto(`${baseUrl}/`, { waitUntil: 'load' });
    await expect(page.locator('#mobile')).toBeVisible();
    await expect(page.locator('#mobile')).toContainText('not intended for mobile');
    expect(await page.evaluate(() => document.body.dataset.state)).toBe('confirm');
    // Nothing is fetched while the warning is up.
    await page.waitForTimeout(1500);
    expect(wasmRequests).toEqual([]);

    await page.locator('#continue').click();
    await page.waitForFunction(() => document.body.dataset.state === 'ready', undefined, { timeout: 90_000 });
    expect(wasmRequests).toEqual([`${baseUrl}/assets/${manifest.bundleId}/plotjuggler4.wasm`]);
    expect(pageErrors).toEqual([]);
  } finally {
    await context.close();
  }
});
