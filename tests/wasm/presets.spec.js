// SPDX-License-Identifier: MPL-2.0
// A preset is an ordinary source-bound layout opened by URL: its file sources
// download from beside it, with no picker, all or nothing. A deployment lists
// its presets in presets/index.json beside the page; the welcome dialog offers
// them (and "Upload your own…"), and ?preset=<id> opens one directly.
const { test, expect } = require('@playwright/test');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { observeRuntimeTarget, replaceRuntimeTarget, waitForQtApp, wasmHeap } = require('./support/app');
const { count, latest } = require('./support/console');
const { clickActiveDialogButton } = require('./support/dialogs');
const { startHostedAssetMirror } = require('./support/hosted_assets');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { openFileChooser } = require('./support/pickers');
const { requestMultiPlotState } = require('./support/plot_probes');
const { datasetCount, dismissDialogs, openPreset, startPresetServer } = require('./support/preset_server');
const { requestScene2DState } = require('./support/scene2d');
const { waitForScene3DModels } = require('./support/scene3d_acceptance');
const { qtPointToCss } = require('./support/scene3d_foundation');
const { downloadSourceLayoutFromFileMenu } = require('./support/source_layout');

// Same basename and the same columns: only where each file lives tells them apart.
const left = Buffer.from('time,value\n0,1\n1,2\n2,3\n');
const right = Buffer.from('time,value\n0,8\n1,9\n2,10\n');
const sha256 = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

function attributes(xml, tag, attribute) {
  return [...xml.matchAll(new RegExp(`<${tag}\\b[^>]*\\b${attribute}="([^"]*)"`, 'g'))].map(match => match[1]);
}

// Sets `attribute` on the index-th `tag` element.
function setAttributeAt(xml, tag, index, attribute, value) {
  let current = 0;
  return xml.replace(new RegExp(`<${tag}\\b[^>]*>`, 'g'), (element) => {
    if (current++ !== index) {
      return element;
    }
    return element.replace(new RegExp(`\\b${attribute}="[^"]*"`), `${attribute}="${value}"`);
  });
}

async function boot(page, messages, errors) {
  observeRuntimeTarget(page, messages, [], errors);
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, messages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return screen;
}

async function loadCsv(page, screen, messages, bytes) {
  const loaded = count(messages, 'PJ_FILE_LOAD_OK');
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({ name: 'duplicate.csv', mimeType: 'text/csv', buffer: bytes });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, messages, 'ok');
  await expect.poll(() => count(messages, 'PJ_FILE_LOAD_OK'), { timeout: 15000 }).toBeGreaterThan(loaded);
}

// The app's own source-bound layout for the two CSVs, as a preset author gets it.
async function authorPresetLayout(page, screen, messages) {
  await loadCsv(page, screen, messages, left);
  await loadCsv(page, screen, messages, right);
  const layout = (await downloadSourceLayoutFromFileMenu(page, screen, messages)).bytes.toString('utf8');
  expect(attributes(layout, 'fileInfo', 'filename')).toEqual([
    'pj4-sources/0/duplicate.csv',
    'pj4-sources/1/duplicate.csv',
  ]);
  expect(attributes(layout, 'plugin', 'manifest_id')).toHaveLength(2);
  expect(layout).toContain('<DockArea');
  return layout;
}

async function workspaceSnapshot(page, messages) {
  const before = count(messages, 'PJ_WASM_LAYOUT_SNAPSHOT base64=');
  await page.evaluate(() => window.pjWasmReportLayoutSnapshotProbe());
  await expect.poll(() => count(messages, 'PJ_WASM_LAYOUT_SNAPSHOT base64=')).toBeGreaterThan(before);
  return latest(messages, 'PJ_WASM_LAYOUT_SNAPSHOT base64=').match(/base64=([A-Za-z0-9+/=]+)/)[1];
}

// One preset folder; an override of `undefined` leaves that file out.
function presetFiles(folder, layout, overrides = {}) {
  const files = {
    [`${folder}/layout.pj4.xml`]: layout,
    [`${folder}/pj4-sources/0/duplicate.csv`]: left,
    [`${folder}/pj4-sources/1/duplicate.csv`]: right,
    ...overrides,
  };
  return Object.fromEntries(Object.entries(files).filter(([, file]) => file !== undefined));
}

test('a preset loads its relative sources with no picker, duplicate basenames included', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  let messages = [];
  const screen = await boot(page, messages, errors);
  const layout = await authorPresetLayout(page, screen, messages);
  const server = await startPresetServer(presetFiles('pair', layout));
  try {
    // A fresh runtime: none of the authoring uploads or their staged files remain.
    const fresh = await replaceRuntimeTarget(page, [], errors);
    messages = fresh.consoleMessages;
    expect(await openPreset(fresh.page, messages, server.url('pair/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(count(messages, 'PJ_WASM_SOURCE_LAYOUT_DECISION')).toBe(0);
    expect(count(messages, 'PJ_WASM_LAYOUT_PICKER_OPEN')).toBe(0);
    expect(server.requests).toEqual([
      'GET /presets/pair/layout.pj4.xml',
      'GET /presets/pair/pj4-sources/0/duplicate.csv',
      'GET /presets/pair/pj4-sources/1/duplicate.csv',
    ]);
    const loads = messages.filter(message => message.includes('PJ_FILE_LOAD_OK'));
    expect(loads).toHaveLength(2);
    expect(await datasetCount(fresh.page, messages)).toBe(2);

    // Saving keeps the existing contract: logical names plus fingerprints,
    // reselected on the next open; neither staging paths nor download URLs leak.
    const saved = (await downloadSourceLayoutFromFileMenu(fresh.page, fresh.screen, messages)).bytes.toString('utf8');
    expect(attributes(saved, 'fileInfo', 'filename')).toEqual([
      'pj4-sources/0/duplicate.csv',
      'pj4-sources/1/duplicate.csv',
    ]);
    expect(attributes(saved, 'fileInfo', 'content_sha256')).toEqual([sha256(left), sha256(right)]);
    const generic = (await downloadLayoutFromFileMenu(fresh.page, fresh.screen, messages)).bytes.toString('utf8');
    for (const bytes of [saved, generic]) {
      expect(bytes).not.toContain('pj-upload://');
      expect(bytes).not.toContain('/pj_uploads');
      expect(bytes).not.toContain(server.origin);
    }
    expect(errors).toEqual([]);
  } finally {
    await server.close();
  }
});

test('a preset that fails anywhere leaves the workspace as it was', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  let messages = [];
  const screen = await boot(page, messages, errors);
  const layout = await authorPresetLayout(page, screen, messages);
  const otherSha = sha256(Buffer.from('other bytes'));
  const brokenScene = layout.replace(
    /(<DockArea\b[^>]*>)[\s\S]*?(<\/DockArea>)/,
    '$1<scene3d><layer object_type="not_a_type"/></scene3d>$2',
  );
  const missingPlugin = setAttributeAt(
    setAttributeAt(layout, 'plugin', 1, 'manifest_id', 'pj-missing-plugin'), 'plugin', 1, 'ID', 'Missing Plugin');
  const server = await startPresetServer({
    ...presetFiles('good', layout),
    // The second file is absent.
    ...presetFiles('missing', layout, { 'missing/pj4-sources/1/duplicate.csv': undefined }),
    // The second file is not the one the layout was saved with.
    ...presetFiles('mismatch', setAttributeAt(layout, 'fileInfo', 1, 'content_sha256', otherSha)),
    // The second fingerprint is unreadable: refused before anything downloads.
    ...presetFiles('malformed', setAttributeAt(layout, 'fileInfo', 1, 'content_sha256', 'ABCDEF')),
    // The second file's host does not allow this page to read it.
    ...presetFiles('denied', layout, { 'denied/pj4-sources/1/duplicate.csv': { body: right, cors: false } }),
    // The second file names a plugin this build lacks, after the first imported.
    ...presetFiles('import', missingPlugin),
    // Both import, then the layout itself cannot apply.
    ...presetFiles('apply', brokenScene),
  });
  try {
    const fresh = await replaceRuntimeTarget(page, [], errors);
    messages = fresh.consoleMessages;
    expect(await openPreset(fresh.page, messages, server.url('good/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(await datasetCount(fresh.page, messages)).toBe(2);
    const workspace = await workspaceSnapshot(fresh.page, messages);
    const loadsBefore = count(messages, 'PJ_FILE_LOAD_OK');

    for (const [folder, reason] of [
      ['missing', 'download'],
      ['mismatch', 'fingerprint'],
      ['malformed', 'fingerprint'],
      ['denied', 'download'],
      ['import', 'import'],
      ['apply', 'apply-after-source-load'],
    ]) {
      const outcome = await openPreset(fresh.page, messages, server.url(`${folder}/layout.pj4.xml`));
      expect(outcome, folder).toContain('PJ_WASM_LAYOUT_LOAD_FAILED');
      expect(outcome, folder).toContain(`reason=${reason}`);
      await dismissDialogs(fresh.page, messages);
      expect(await datasetCount(fresh.page, messages), folder).toBe(2);
      expect(await workspaceSnapshot(fresh.page, messages), folder).toBe(workspace);
    }
    expect(server.requests).not.toContain('GET /presets/malformed/pj4-sources/0/duplicate.csv');
    // Only the import case (its first source) and the apply case (both) got as
    // far as importing, and every one of those datasets is gone again.
    expect(count(messages, 'PJ_FILE_LOAD_OK') - loadsBefore).toBe(3);
    expect(errors).toEqual([]);
  } finally {
    await server.close();
  }
});

const APP_URL = process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html';
// A sample load may stall the page's main thread no longer than this.
const MAX_FRAME_GAP_MS = 2000;
const INDEX_URL = new URL('presets/index.json', APP_URL).href;
// A 1x1 PNG thumbnail.
const PNG = Buffer.from(
  'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==', 'base64');

// A fresh runtime at APP_URL + `query` whose presets/index.json answers `index`
// (an object as JSON, a string verbatim; undefined leaves the deployment's own
// 404). Returns once the app has finished looking for presets.
async function bootWithIndex(context, errors, { index, query = '' } = {}) {
  const page = await context.newPage();
  // Keeps file-chooser interception on before any picker can open.
  page.on('filechooser', () => {});
  if (index !== undefined) {
    await page.route(INDEX_URL, route => route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: typeof index === 'string' ? index : JSON.stringify(index),
    }));
  }
  const messages = [];
  observeRuntimeTarget(page, messages, [], errors);
  const url = new URL(APP_URL);
  url.search = query;
  await page.goto(url.href);
  await waitForQtApp(page, messages);
  await expect.poll(() => count(messages, 'PJ_WASM_PRESETS_DISCOVERED'), { timeout: 30000 }).toBe(1);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return { page, messages, screen };
}

function point(text) {
  const [x, y] = text.split(',').map(Number);
  return { x, y };
}

// The welcome dialog as the user sees it: visibility, button centers (-1,-1
// when hidden), the status line and every entry with its thumbnail state.
async function presetsDialog(page, messages) {
  const before = messages.length;
  await page.evaluate(() => window.pjWasmReportPresetsDialogProbe());
  const done = () => messages.slice(before).find(message => message.includes('PJ_WASM_PRESETS_DIALOG visible='));
  await expect.poll(() => Boolean(done()), { timeout: 10000 }).toBe(true);
  const entries = {};
  for (const message of messages.slice(before)) {
    const entry = message.match(/PJ_WASM_PRESETS_ENTRY id=(\S+) thumbnail=(\S+) open=(-?\d+,-?\d+) enabled=(\d)/);
    if (entry) {
      entries[entry[1]] = { thumbnail: entry[2], open: point(entry[3]), enabled: entry[4] === '1' };
    }
  }
  const dialog = done().match(/visible=(\d) cancel=(-?\d+,-?\d+) retry=(-?\d+,-?\d+) upload=(-?\d+,-?\d+) status=([^\r\n]*)/);
  expect(dialog, `unparseable presets dialog report: ${done()}`).not.toBeNull();
  return {
    visible: dialog[1] === '1',
    cancel: point(dialog[2]),
    retry: point(dialog[3]),
    upload: point(dialog[4]),
    status: dialog[5],
    entries,
  };
}

async function click(run, target) {
  expect(target.x, 'the control is not visible').toBeGreaterThanOrEqual(0);
  await run.page.mouse.click(run.screen.x + target.x, run.screen.y + target.y);
}

// Triggers File → Presets through its action and reports whether it was offered.
async function presetsAction(page, messages) {
  const before = messages.length;
  await page.evaluate(() => window.pjWasmOpenPresetsProbe());
  const report = () => messages.slice(before).find(message => message.includes('PJ_WASM_PRESETS_ACTION'));
  await expect.poll(() => Boolean(report()), { timeout: 10000 }).toBe(true);
  return report();
}

// The PJ_WASM_LAYOUT_LOAD_* outcome of `url` logged after message `from`.
function urlOutcome(messages, from, url) {
  return messages.slice(from).find(
    message => /PJ_WASM_(LAYOUT_LOAD_(OK|FAILED)|SOURCE_LAYOUT_CANCELLED) name=/.test(message) &&
      message.includes(`name=${url}`),
  ) || '';
}

test('the welcome dialog lists the presets, keeps broken thumbnails on a placeholder and opens one', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const messages = [];
  const screen = await boot(page, messages, errors);
  // No index is deployed here: the dialog never opened and File → Presets is not offered.
  await expect.poll(() => count(messages, 'PJ_WASM_PRESETS_DISCOVERED count=0'), { timeout: 30000 }).toBe(1);
  const layout = await authorPresetLayout(page, screen, messages);
  const server = await startPresetServer({
    ...presetFiles('pair', layout),
    'pair/thumbnail.png': PNG,
    'broken/thumbnail.png': Buffer.from('not an image'),
  });
  try {
    const context = page.context();
    await page.close();
    const pairUrl = server.url('pair/layout.pj4.xml');
    const missingUrl = server.url('missing/layout.pj4.xml');
    const run = await bootWithIndex(context, errors, {
      index: {
        presets: [
          { id: 'pair', title: 'Pair', description: 'Two CSV files.', layout: pairUrl, thumbnail: server.url('pair/thumbnail.png') },
          { id: 'broken', title: 'Broken thumbnail', layout: pairUrl, thumbnail: server.url('broken/thumbnail.png') },
          { id: 'missing', title: 'Missing', layout: missingUrl, thumbnail: server.url('missing/thumbnail.png') },
        ],
      },
    });
    expect(count(run.messages, 'PJ_WASM_PRESETS_DISCOVERED count=3')).toBe(1);

    // The dialog opens by itself once the window is up; thumbnails that fail
    // to download or decode leave their entry on the placeholder, usable.
    let dialog;
    await expect.poll(async () => {
      dialog = await presetsDialog(run.page, run.messages);
      return dialog.visible && Object.values(dialog.entries).every(entry => entry.thumbnail !== 'loading');
    }, { timeout: 30000 }).toBe(true);
    expect(Object.keys(dialog.entries)).toEqual(['pair', 'broken', 'missing']);
    expect(Object.fromEntries(Object.entries(dialog.entries).map(([id, entry]) => [id, entry.thumbnail]))).toEqual({
      pair: 'image', broken: 'placeholder', missing: 'placeholder',
    });
    expect(Object.values(dialog.entries).every(entry => entry.enabled)).toBe(true);

    // A preset that fails says so in the dialog and can be retried.
    let from = run.messages.length;
    await click(run, dialog.entries.missing.open);
    await expect.poll(() => urlOutcome(run.messages, from, missingUrl), { timeout: 30000 }).toContain('reason=download');
    await dismissDialogs(run.page, run.messages);
    dialog = await presetsDialog(run.page, run.messages);
    expect(dialog.visible).toBe(true);
    expect(dialog.status).toContain('Missing');
    from = run.messages.length;
    await click(run, dialog.retry);
    await expect.poll(() => urlOutcome(run.messages, from, missingUrl), { timeout: 30000 }).toContain('reason=download');
    await dismissDialogs(run.page, run.messages);

    // Picking a sample loads its data and its layout, then the dialog closes.
    dialog = await presetsDialog(run.page, run.messages);
    from = run.messages.length;
    await click(run, dialog.entries.pair.open);
    await expect.poll(() => urlOutcome(run.messages, from, pairUrl), { timeout: 30000 }).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(await datasetCount(run.page, run.messages)).toBe(2);
    await expect.poll(async () => (await presetsDialog(run.page, run.messages)).visible).toBe(false);

    // File → Presets reopens it; "Upload your own…" closes it and opens the data-file picker.
    expect(await presetsAction(run.page, run.messages)).toContain('present=1 visible=1 enabled=1');
    await expect.poll(async () => (dialog = await presetsDialog(run.page, run.messages)).visible).toBe(true);
    const chooserPromise = run.page.waitForEvent('filechooser', { timeout: 15000 });
    await click(run, dialog.upload);
    const chooser = await chooserPromise;
    expect((await presetsDialog(run.page, run.messages)).visible).toBe(false);
    const loaded = count(run.messages, 'PJ_FILE_LOAD_OK');
    await chooser.setFiles({ name: 'upload.csv', mimeType: 'text/csv', buffer: left });
    await run.page.waitForTimeout(1000);
    await clickActiveDialogButton(run.page, run.screen, run.messages, 'ok');
    await expect.poll(() => count(run.messages, 'PJ_FILE_LOAD_OK'), { timeout: 15000 }).toBeGreaterThan(loaded);
    expect(await datasetCount(run.page, run.messages)).toBe(3);
    expect(count(run.messages, 'PJ_PRESET_INDEX_INVALID')).toBe(0);
    expect(errors).toEqual([]);
  } finally {
    await server.close();
  }
});

test('without a usable preset index the app starts as before', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const context = page.context();
  await page.close();
  for (const [name, index] of [
    ['missing', undefined],
    ['empty', { presets: [] }],
    ['malformed', '{"presets": [{"id": "orphan"}]}'],
  ]) {
    const run = await bootWithIndex(context, errors, { index });
    expect(count(run.messages, 'PJ_WASM_PRESETS_DISCOVERED count=0'), name).toBe(1);
    expect((await presetsDialog(run.page, run.messages)).visible, name).toBe(false);
    expect(await presetsAction(run.page, run.messages), name).toContain('present=1 visible=0');
    // Only a malformed index is worth one warning, naming the problem.
    const warnings = run.messages.filter(message => message.includes('PJ_PRESET_INDEX_INVALID'));
    expect(warnings, name).toHaveLength(name === 'malformed' ? 1 : 0);
    if (name === 'malformed') {
      expect(warnings[0]).toContain('orphan');
    }
    await run.page.close();
  }

  // A deep link to a page with no presets says so and loads nothing.
  const linked = await bootWithIndex(context, errors, { query: '?preset=pair' });
  await expect.poll(async () => {
    await linked.page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
    return latest(linked.messages, 'PJ_WASM_LAYOUT_DIALOG present=');
  }).toContain('present=1');
  expect(latest(linked.messages, 'PJ_WASM_LAYOUT_DIALOG present=')).toContain('pair');
  expect((await presetsDialog(linked.page, linked.messages)).visible).toBe(false);
  expect(count(linked.messages, 'PJ_WASM_LAYOUT_LOAD_')).toBe(0);
  expect(errors).toEqual([]);
});

test('a ?preset= deep link opens that preset directly; an unknown or repeated one loads nothing', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const messages = [];
  const screen = await boot(page, messages, errors);
  const layout = await authorPresetLayout(page, screen, messages);
  const server = await startPresetServer(
    presetFiles('pair', layout, { 'pair/pj4-sources/1/duplicate.csv': { body: right, hold: true } }));
  try {
    const context = page.context();
    await page.close();
    const pairUrl = server.url('pair/layout.pj4.xml');
    const index = {
      presets: [
        { id: 'pair', title: 'Pair', layout: pairUrl },
        { id: 'other', title: 'Other', layout: server.url('other/layout.pj4.xml') },
      ],
    };
    const held = 'GET /presets/pair/pj4-sources/1/duplicate.csv';

    // No selection step: the load starts by itself, its progress and Cancel in view.
    const run = await bootWithIndex(context, errors, { index, query: '?preset=pair' });
    await expect.poll(() => server.requests.filter(request => request === held).length, { timeout: 30000 }).toBe(1);
    let dialog = await presetsDialog(run.page, run.messages);
    expect(dialog.visible).toBe(true);
    expect(dialog.cancel.x).toBeGreaterThanOrEqual(0);
    expect(dialog.entries.pair.enabled).toBe(false);
    expect(dialog.status).toContain('Pair');

    // Cancel stops it and gives the list back; picking it again loads it.
    let from = run.messages.length;
    await click(run, dialog.cancel);
    await expect.poll(() => urlOutcome(run.messages, from, pairUrl), { timeout: 30000 })
      .toContain('PJ_WASM_SOURCE_LAYOUT_CANCELLED');
    await expect.poll(async () => (dialog = await presetsDialog(run.page, run.messages)).entries.pair.enabled).toBe(true);
    expect(dialog.cancel.x).toBe(-1);
    expect(await datasetCount(run.page, run.messages)).toBe(0);
    from = run.messages.length;
    await click(run, dialog.entries.pair.open);
    await expect.poll(() => server.requests.filter(request => request === held).length, { timeout: 30000 }).toBe(2);
    server.release();
    await expect.poll(() => urlOutcome(run.messages, from, pairUrl), { timeout: 30000 }).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(await datasetCount(run.page, run.messages)).toBe(2);
    await expect.poll(async () => (await presetsDialog(run.page, run.messages)).visible).toBe(false);
    await run.page.close();

    // An unknown id or a repeated key is reported in the dialog and fetches nothing.
    const requests = server.requests.length;
    for (const query of ['?preset=nope', '?preset=pair&preset=other']) {
      const refused = await bootWithIndex(context, errors, { index, query });
      await expect.poll(async () => (await presetsDialog(refused.page, refused.messages)).visible, { timeout: 10000 })
        .toBe(true);
      dialog = await presetsDialog(refused.page, refused.messages);
      expect(dialog.status, query).not.toBe('');
      expect(dialog.cancel.x, query).toBe(-1);
      expect(Object.values(dialog.entries).every(entry => entry.enabled), query).toBe(true);
      await refused.page.waitForTimeout(500);
      expect(count(refused.messages, 'PJ_WASM_LAYOUT_LOAD_'), query).toBe(0);
      await refused.page.close();
    }
    expect(server.requests.length).toBe(requests);
    expect(errors).toEqual([]);
  } finally {
    await server.close();
  }
});

// The deployed samples (resources/wasm-presets) in the acceptance package that
// carries them (scripts/package_wasm.mjs --presets), served like production
// from presets/ beside the page. Every other spec boots the plain package, so
// no welcome dialog sits over its window.
const SAMPLES_PACKAGE = path.resolve(
  process.env.PJ_WASM_PRESETS_PACKAGE_DIR || path.join(__dirname, '..', '..', 'build-wasm-probe', 'deploy-presets'));
const SAMPLE_DIRECTORY = path.join(__dirname, '..', '..', 'resources', 'wasm-presets');
const SAMPLE_IDS = ['anymal', 'city_drive', 'signals'];

async function startSamples() {
  if (!fs.existsSync(path.join(SAMPLES_PACKAGE, 'manifest.json'))) {
    throw new Error(`No sample package at ${SAMPLES_PACKAGE}: package it with `
      + 'node scripts/package_wasm.mjs --presets resources/wasm-presets --source <pj_app> --output <it>');
  }
  const { listenWasmServer } = await import('../../scripts/serve_wasm.mjs');
  const server = await listenWasmServer(SAMPLES_PACKAGE);
  const origin = `http://127.0.0.1:${server.address().port}`;
  const mirror = await startHostedAssetMirror();
  return {
    origin,
    mirror,
    url: file => `${origin}/presets/${file}`,
    close: async () => {
      await mirror.close();
      await new Promise((resolve, reject) => server.close(error => (error ? reject(error) : resolve())));
    },
  };
}

// Contexts already sending the samples' hosted files to the mirror (a test boots several pages in one).
const ROUTED_CONTEXTS = new WeakSet();

async function bootSamples(context, errors, samples) {
  if (!ROUTED_CONTEXTS.has(context)) {
    ROUTED_CONTEXTS.add(context);
    await samples.mirror.route(context, samples.origin);
  }
  const page = await context.newPage();
  page.on('filechooser', () => {});
  const messages = [];
  observeRuntimeTarget(page, messages, [], errors);
  await page.goto(`${samples.origin}/`);
  await waitForQtApp(page, messages);
  await expect.poll(() => count(messages, 'PJ_WASM_PRESETS_DISCOVERED'), { timeout: 30000 }).toBe(1);
  expect(latest(messages, 'PJ_WASM_PRESETS_DISCOVERED')).toContain(`count=${SAMPLE_IDS.length}`);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return { page, messages, screen };
}

// Opens a sample from the welcome dialog (through File → Presets once it has
// closed) and returns its PJ_WASM_LAYOUT_LOAD_* outcome.
async function pickSample(run, samples, id) {
  let dialog = await presetsDialog(run.page, run.messages);
  if (!dialog.visible) {
    expect(await presetsAction(run.page, run.messages)).toContain('present=1 visible=1 enabled=1');
  }
  await expect.poll(async () => {
    dialog = await presetsDialog(run.page, run.messages);
    return dialog.visible && Boolean(dialog.entries[id]?.enabled);
  }, { timeout: 15000 }).toBe(true);
  const url = samples.url(`${id}/layout.pj4.xml`);
  const from = run.messages.length;
  await click(run, dialog.entries[id].open);
  await expect.poll(() => urlOutcome(run.messages, from, url), { timeout: 90000 }).not.toBe('');
  return urlOutcome(run.messages, from, url);
}

// A dock writes its TF topics in the reverse of the order it restored them in, so
// workspaces are compared with every run of <config_topic> elements sorted.
function comparableWorkspace(base64) {
  return Buffer.from(base64, 'base64').toString('utf8').replace(
    /(?:[ \t]*<config_topic\b[^>]*\/>\n)+/g, run => `${run.split('\n').filter(Boolean).sort().join('\n')}\n`);
}

async function stagedFiles(page, messages) {
  const before = count(messages, 'PJ_WASM_STAGED_FILES count=');
  await page.evaluate(() => window.pjWasmReportStagedFilesProbe());
  await expect.poll(() => count(messages, 'PJ_WASM_STAGED_FILES count=')).toBeGreaterThan(before);
  return Number(latest(messages, 'PJ_WASM_STAGED_FILES count=').match(/count=(\d+)/)[1]);
}

// Every plot of the sample's tab with the curves it names, drawn.
async function expectPlots(run, expected) {
  let state;
  await expect.poll(async () => {
    state = await requestMultiPlotState(run.page, run.messages, count(run.messages, 'PJ_WASM_MULTI_SUMMARY'));
    return state.plots.map(plot => plot.titles);
  }, { timeout: 30000 }).toEqual(expected);
  for (const plot of state.plots) {
    expect(plot.vertices, plot.titles.join(',')).toBeGreaterThan(0);
    expect(plot.readback.curves.some(curve => curve.pixels > 0), JSON.stringify(plot.readback.curves)).toBe(true);
  }
}

// Seeks the global timeline to `ratio` of its range.
async function seek(run, state, ratio) {
  const { slider } = state;
  const at = value => qtPointToCss(run.screen, state, {
    x: slider.x + 4 + value * (slider.width - 8),
    y: slider.y + slider.height / 2,
  });
  const from = at((slider.value - slider.min) / Math.max(1, slider.max - slider.min));
  await run.page.mouse.move(from.x, from.y);
  await run.page.mouse.down();
  await run.page.mouse.move(at(ratio).x, at(ratio).y, { steps: 12 });
  await run.page.mouse.up();
}

// The 3D view draws what `ready` describes, and moving between two instants
// inside the recording changes what it draws. Neither is the first instant, which
// may precede the first transform or a layer's first sample (a cloud draws nothing
// before its first scan): a robot posed once and then frozen fails.
async function expectSceneMoves(run, ready) {
  const start = await waitForScene3DModels(run.page, run.messages, () => true);
  await seek(run, start, 0.25);
  const first = await waitForScene3DModels(run.page, run.messages, (models, dock, state) =>
    ready(models, dock, state) && state.slider.value > state.slider.min, 60000);
  await seek(run, first, 0.75);
  return waitForScene3DModels(run.page, run.messages, (models, dock, state) =>
    ready(models, dock, state) && state.slider.value > first.slider.value
      && dock.readback.frameHash !== first.docks[0].readback.frameHash);
}

// The ANYmal and the GrandTour payload with every mesh fetched and the model pass drawing them:
// unlike the scan, they do not depend on where the cursor is.
const anymalLoaded = models => models.robots.live === 2 && models.robots.placeholders === 0
  && models.robots.draws > 0 && models.robots.status.length === 2
  && models.robots.status.every(status => /\b(\d+)\/\1 meshes/.test(status))
  && models.models.rendered > 0 && models.models.draws >= models.robots.draws;

// The robots and, around them, the lidar scan the recording carries compressed.
const anymalReady = (models, dock) => anymalLoaded(models)
  && dock.data.points.live === 1 && dock.data.points.compressed === 1 && dock.data.points.rendered > 0;

// The ANYmal sample's plots, its three camera views, and its robots moving inside its scan.
async function expectAnymalViews(run) {
  await expectPlots(run, [
    ['LF', 'RF', 'LH', 'RH'].map(leg => `joint_states/${leg}_KFE/position`),
    ['x', 'y', 'z'].map(axis => `anymal/imu/angular_velocity/${axis}`),
    ['x', 'y'].map(axis => `leg_odometry/odom/twist/twist/linear/${axis}`),
  ]);
  const cameras = await requestScene2DState(run.page, run.messages, count(run.messages, 'PJ_WASM_SCENE2D_SUMMARY'));
  expect(cameras.docks.map(dock => [dock.layers, dock.visible])).toEqual([[1, true], [1, true], [1, true]]);
  await expectSceneMoves(run, anymalReady);
}

// The lidar scan (Cloudini-compressed) restored in the map frame and drawn. Only once the cursor
// has passed the first scan, a few tens of milliseconds into the clip.
const cityDriveReady = (_models, dock) => dock.fixed === 'map' && dock.data.points.live === 1
  && dock.data.points.compressed === 1 && dock.data.points.rendered > 0;

// The City drive sample's plots, its front camera view, and the lidar scan following the car.
async function expectCityDriveViews(run) {
  await expectPlots(run, [
    ['applanix/odom/twist/twist/linear/y'],
    ['applanix/imu/angular_velocity/z'],
    ['y', 'x'].map(axis => `applanix/imu/linear_acceleration/${axis}`),
  ]);
  const cameras = await requestScene2DState(run.page, run.messages, count(run.messages, 'PJ_WASM_SCENE2D_SUMMARY'));
  expect(cameras.docks.map(dock => [dock.layers, dock.visible])).toEqual([[1, true]]);
  await expectSceneMoves(run, cityDriveReady);
}

async function expectSignalsViews(run) {
  await expectPlots(run, [['data/sine', 'data/cosine', 'data/square'], ['data/sawtooth', 'data/chirp', 'data/damped']]);
}

test('every deployed sample opens into its own views', async ({ page }) => {
  test.setTimeout(360000);
  const errors = [];
  const samples = await startSamples();
  try {
    const context = page.context();
    await page.close();
    for (const [id, expectViews] of [
      ['anymal', expectAnymalViews], ['city_drive', expectCityDriveViews], ['signals', expectSignalsViews],
    ]) {
      const run = await bootSamples(context, errors, samples);
      let dialog;
      await expect.poll(async () => {
        dialog = await presetsDialog(run.page, run.messages);
        return dialog.visible && Object.values(dialog.entries).every(entry => entry.thumbnail !== 'loading');
      }, { timeout: 30000 }).toBe(true);
      expect(Object.keys(dialog.entries)).toEqual(SAMPLE_IDS);
      expect(Object.values(dialog.entries).map(entry => entry.thumbnail), id).toEqual(SAMPLE_IDS.map(() => 'image'));

      expect(await pickSample(run, samples, id), id).toContain('PJ_WASM_LAYOUT_LOAD_OK');
      await expect.poll(async () => (await presetsDialog(run.page, run.messages)).visible, { timeout: 10000 })
        .toBe(false);
      await expectViews(run);
      expect(await datasetCount(run.page, run.messages), id).toBe(1);
      expect(count(run.messages, 'PJ_PRESET_INDEX_INVALID'), id).toBe(0);
      await run.page.close();
    }
    expect(errors).toEqual([]);
  } finally {
    await samples.close();
  }
});

// @serial: its frame-gap bound fails on a loaded runner (playwright.config.js).
test('anymal sample stays responsive through load and reopen', { tag: '@serial' }, async ({ page }) => {
  test.setTimeout(300000);
  const errors = [];
  const samples = await startSamples();
  try {
    const context = page.context();
    await page.close();
    const run = await bootSamples(context, errors, samples);
    // The longest the page's main thread goes without painting a frame while `load` runs.
    const longestFrameGap = async (load) => {
      await run.page.evaluate(() => {
        const watch = { last: performance.now(), longest: 0, running: true };
        const tick = (now) => {
          watch.longest = Math.max(watch.longest, now - watch.last);
          watch.last = now;
          if (watch.running) {
            requestAnimationFrame(tick);
          }
        };
        requestAnimationFrame(tick);
        window.pjSampleFrameWatch = watch;
      });
      await load();
      return run.page.evaluate(() => {
        window.pjSampleFrameWatch.running = false;
        return window.pjSampleFrameWatch.longest;
      });
    };
    const openAnymal = async (pass, staged) => {
      const longest = await longestFrameGap(async () => {
        expect(await pickSample(run, samples, 'anymal'), pass).toContain('PJ_WASM_LAYOUT_LOAD_OK');
        await waitForScene3DModels(run.page, run.messages, anymalLoaded, 60000);
      });
      console.log(`anymal ${pass}: longest frame gap ${Math.round(longest)} ms`);
      expect(longest, pass).toBeLessThan(MAX_FRAME_GAP_MS);
      // The open ANYmal recording keeps its staged file for its lazy MCAP reader; a
      // replaced sample releases its own, and the CSV parses eagerly and lets
      // its file go, so nothing else is left behind.
      expect(await stagedFiles(run.page, run.messages), pass).toBe(staged);
    };

    await openAnymal('load', 1);
    // Switching to another sample brings its views and replaces the one it
    // switched from; reopening ANYmal brings its robot back.
    expect(await pickSample(run, samples, 'signals')).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expectSignalsViews(run);
    expect(await datasetCount(run.page, run.messages)).toBe(1);
    await openAnymal('reopen', 1);
    expect(await datasetCount(run.page, run.messages)).toBe(1);
    await expectAnymalViews(run);
    expect(errors).toEqual([]);
  } finally {
    await samples.close();
  }
});

test('a sample that fails releases what it staged and leaves the open sample as it was', { tag: '@serial' }, async ({ page }) => {
  test.setTimeout(300000);
  const errors = [];
  const samples = await startSamples();
  try {
    const context = page.context();
    await page.close();
    const run = await bootSamples(context, errors, samples);
    // The open sample is the one holding a staged file (its lazy MCAP reader).
    expect(await pickSample(run, samples, 'anymal')).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await waitForScene3DModels(run.page, run.messages, anymalLoaded, 60000);
    // Its camera decoders are running before a failure tears a load down.
    const cameras = await requestScene2DState(run.page, run.messages, count(run.messages, 'PJ_WASM_SCENE2D_SUMMARY'));
    expect(cameras.docks.map(dock => [dock.layers, dock.visible])).toEqual([[1, true], [1, true], [1, true]]);
    const workspace = comparableWorkspace(await workspaceSnapshot(run.page, run.messages));
    const staged = await stagedFiles(run.page, run.messages);

    const signalsCsv = fs.readFileSync(path.join(SAMPLE_DIRECTORY, 'signals', 'data.csv'));
    const signalsLayout = fs.readFileSync(path.join(SAMPLE_DIRECTORY, 'signals', 'layout.pj4.xml'), 'utf8');
    for (const [file, body, reason] of [
      // Staged, then refused: these bytes are not the file the layout names.
      ['signals/data.csv', Buffer.concat([signalsCsv, Buffer.from('9.98,0,1,1,1,0,1\n')]), 'fingerprint'],
      // Imported, then rolled back: the layout cannot apply.
      ['signals/layout.pj4.xml', signalsLayout.replace(
        /(<DockArea\b[^>]*>)[\s\S]*?(<\/DockArea>)/, '$1<scene3d><layer object_type="not_a_type"/></scene3d>$2'),
      'apply-after-source-load'],
    ]) {
      const url = samples.url(file);
      await run.page.route(url, route => route.fulfill({ status: 200, body }));
      const outcome = await pickSample(run, samples, 'signals');
      await run.page.unroute(url);
      expect(outcome, file).toContain('PJ_WASM_LAYOUT_LOAD_FAILED');
      expect(outcome, file).toContain(`reason=${reason}`);
      await dismissDialogs(run.page, run.messages);
      await expect.poll(() => stagedFiles(run.page, run.messages), { timeout: 15000 }).toBe(staged);
      expect(await datasetCount(run.page, run.messages), file).toBe(1);
      expect(comparableWorkspace(await workspaceSnapshot(run.page, run.messages)), file).toBe(workspace);
    }
    // Served its own files again, it opens in place of the open sample.
    expect(await pickSample(run, samples, 'signals')).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expectSignalsViews(run);
    expect(await datasetCount(run.page, run.messages)).toBe(1);
    expect(errors).toEqual([]);
  } finally {
    await samples.close();
  }
});

// A recording streams from the network into its staged file, never through the
// wasm heap: the download leaves no copy behind and peaks at no copy on the way.
// Served the wrong bytes, a recording takes the whole download and staging path
// and is refused before any import, so no parsed data blurs the figures. The
// bounds sit far below one copy of the recording, yet above allocator noise and
// one 20 % growth step of the linear memory.
test('a preset recording downloads without passing through the wasm heap', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const messages = [];
  await boot(page, messages, errors);
  const recordingBytes = 96 * 1024 * 1024;
  const server = await startPresetServer({
    'big/layout.pj4.xml': fs.readFileSync(path.join(SAMPLE_DIRECTORY, 'signals', 'layout.pj4.xml')),
    'big/data.csv': Buffer.alloc(recordingBytes, '0'),
  });
  try {
    const before = await wasmHeap(page, messages);
    expect(await openPreset(page, messages, server.url('big/layout.pj4.xml'))).toContain('reason=fingerprint');
    expect(server.requests).toEqual(['GET /presets/big/layout.pj4.xml', 'GET /presets/big/data.csv']);
    await dismissDialogs(page, messages);

    const after = await wasmHeap(page, messages);
    console.log(`heap in use ${before.inUse} -> ${after.inUse}, memory ${before.memory} -> ${after.memory}`);
    expect.soft(after.inUse - before.inUse).toBeLessThan(recordingBytes / 4);
    expect.soft(after.memory - before.memory).toBeLessThan(recordingBytes);
    // The browser keeps a smaller resident payload window than the desktop's 256 MiB.
    expect.soft(after.pool).toBe(64 * 1024 * 1024);
    expect(errors).toEqual([]);
  } finally {
    await server.close();
  }
});

// A 2xx answer without a body (204) is an empty recording: refused for its
// fingerprint like any other wrong bytes, never as a download failure.
test('a recording answered with no body is refused for its fingerprint', async ({ page }) => {
  test.setTimeout(120000);
  const errors = [];
  const messages = [];
  await boot(page, messages, errors);
  const server = await startPresetServer({
    'empty/layout.pj4.xml': fs.readFileSync(path.join(SAMPLE_DIRECTORY, 'signals', 'layout.pj4.xml')),
    'empty/data.csv': { body: '', status: 204 },
  });
  try {
    expect(await openPreset(page, messages, server.url('empty/layout.pj4.xml'))).toContain('reason=fingerprint');
    await dismissDialogs(page, messages);
    expect(errors).toEqual([]);
  } finally {
    await server.close();
  }
});
