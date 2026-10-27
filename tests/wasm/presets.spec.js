// SPDX-License-Identifier: MPL-2.0
// A preset is an ordinary source-bound layout opened by URL: its file sources
// download from beside it, with no picker, all or nothing. A deployment lists
// its presets in presets/index.json beside the page; the welcome dialog offers
// them (and "Upload your own…"), and ?preset=<id> opens one directly.
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');
const {
  bootApp, observeRuntimeTarget, replaceRuntimeTarget, waitForQtApp, wasmHeap,
} = require('./support/app');
const { count, latest } = require('./support/console');
const { clickActiveDialogButton } = require('./support/dialogs');
const { startHostedAssetMirror } = require('./support/hosted_assets');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { elementAttributes, setElementAttributeAt } = require('./support/layout_xml');
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
  expect(elementAttributes(layout, 'fileInfo', 'filename')).toEqual([
    'pj4-sources/0/duplicate.csv',
    'pj4-sources/1/duplicate.csv',
  ]);
  expect(elementAttributes(layout, 'plugin', 'manifest_id')).toHaveLength(2);
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
  const { screen } = await bootApp(page, messages, errors);
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
    expect(elementAttributes(saved, 'fileInfo', 'filename')).toEqual([
      'pj4-sources/0/duplicate.csv',
      'pj4-sources/1/duplicate.csv',
    ]);
    const { sha256 } = await import('../../scripts/wasm_manifest.mjs');
    expect(elementAttributes(saved, 'fileInfo', 'content_sha256')).toEqual([sha256(left), sha256(right)]);
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
  const { screen } = await bootApp(page, messages, errors);
  const layout = await authorPresetLayout(page, screen, messages);
  const { sha256 } = await import('../../scripts/wasm_manifest.mjs');
  const otherSha = sha256(Buffer.from('other bytes'));
  const brokenScene = layout.replace(
    /(<DockArea\b[^>]*>)[\s\S]*?(<\/DockArea>)/,
    '$1<scene3d><layer object_type="not_a_type"/></scene3d>$2',
  );
  const missingPlugin = setElementAttributeAt(
    setElementAttributeAt(layout, 'plugin', 1, 'manifest_id', 'pj-missing-plugin'), 'plugin', 1, 'ID', 'Missing Plugin');
  const server = await startPresetServer({
    ...presetFiles('good', layout),
    // The second file is absent.
    ...presetFiles('missing', layout, { 'missing/pj4-sources/1/duplicate.csv': undefined }),
    // The second file is not the one the layout was saved with.
    ...presetFiles('mismatch', setElementAttributeAt(layout, 'fileInfo', 1, 'content_sha256', otherSha)),
    // The second fingerprint is unreadable: refused before anything downloads.
    ...presetFiles('malformed', setElementAttributeAt(layout, 'fileInfo', 1, 'content_sha256', 'ABCDEF')),
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
// 404); `initScript` runs in the page before its own scripts. Returns once the app
// has finished looking for presets.
async function bootWithIndex(context, errors, { index, query = '', initScript } = {}) {
  const page = await context.newPage();
  if (initScript) {
    await page.addInitScript(initScript);
  }
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
  const { screen } = await bootApp(page, messages, errors);
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
  const { screen } = await bootApp(page, messages, errors);
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

// A deep-linked preset whose layout names its recording by absolute URL, as the deployed samples do.
// `recording` adds to the recording's route; `sha` replaces its recorded fingerprint (null drops it).
// pin(bytes) serves other recording bytes, pinned in the layout.
async function startDeepLinkPreset({ recording = {}, sha } = {}) {
  const layoutRoute = {};
  const recordingRoute = { body: fs.readFileSync(path.join(SIGNALS_FIXTURE, 'data.csv')), ...recording };
  const server = await startPresetServer({
    'signals/layout.pj4.xml': layoutRoute,
    'signals/data.csv': recordingRoute,
  });
  const recordingUrl = server.url('signals/data.csv');
  const layout = fs.readFileSync(path.join(SIGNALS_FIXTURE, 'layout.pj4.xml'), 'utf8').replace(
    /\b(filename|dataset_path|dataset_source)="data\.csv"/g, `$1="${recordingUrl}"`);
  const pinned = value => (value === null
    ? layout.replace(/ content_sha256="[^"]*"/, '')
    : setElementAttributeAt(layout, 'fileInfo', 0, 'content_sha256', value));
  layoutRoute.body = sha === undefined ? layout : pinned(sha);
  const { sha256 } = await import('../../scripts/wasm_manifest.mjs');
  const pin = (bytes) => {
    recordingRoute.body = bytes;
    layoutRoute.body = pinned(sha256(bytes));
    return sha256(bytes);
  };
  const layoutUrl = server.url('signals/layout.pj4.xml');
  const recordingGets = () => server.requests.filter(request => request === 'GET /presets/signals/data.csv');
  // The loading page asked for the recording before the app fetched the layout that names it.
  const fetchedWhileBooting = () => server.requests.indexOf('GET /presets/signals/data.csv')
    < server.requests.lastIndexOf('GET /presets/signals/layout.pj4.xml');
  return {
    server, layoutUrl, recordingUrl, recordingGets, fetchedWhileBooting, pin,
    sha: elementAttributes(layoutRoute.body, 'fileInfo', 'content_sha256')[0],
    index: { presets: [{ id: 'signals', title: 'Signals', layout: layoutUrl }] },
  };
}

// The loading page starts a deep link's recording while the app boots and hands
// the download over: one request, and the replay reads it like its own.
test('a ?preset= deep link downloads its recording once, while the app boots', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const preset = await startDeepLinkPreset();
  try {
    // Without a deep link nothing is fetched ahead.
    const plain = await bootWithIndex(page.context(), errors, { index: preset.index });
    await plain.page.waitForTimeout(2000);
    expect(preset.server.requests).toEqual([]);
    await plain.page.close();

    const run = await bootWithIndex(page.context(), errors, { index: preset.index, query: '?preset=signals' });
    await expect.poll(() => urlOutcome(run.messages, 0, preset.layoutUrl), { timeout: 60000 })
      .toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(await datasetCount(run.page, run.messages)).toBe(1);
    expect(preset.recordingGets()).toHaveLength(1);
    expect(preset.fetchedWhileBooting(), preset.server.requests.join('\n')).toBe(true);
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// A prefetch that fails costs nothing but its request: the app downloads the recording itself.
test('a deep link whose prefetch fails still loads its recording', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const preset = await startDeepLinkPreset({ recording: { corsFailures: 1 } });
  try {
    const run = await bootWithIndex(page.context(), errors, { index: preset.index, query: '?preset=signals' });
    await expect.poll(() => urlOutcome(run.messages, 0, preset.layoutUrl), { timeout: 60000 })
      .toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(await datasetCount(run.page, run.messages)).toBe(1);
    expect(preset.recordingGets()).toHaveLength(2);
    expect(preset.fetchedWhileBooting(), preset.server.requests.join('\n')).toBe(true);
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// A prefetched recording is checked against its fingerprint like any other download.
test('a prefetched recording with the wrong bytes is refused for its fingerprint', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const { sha256 } = await import('../../scripts/wasm_manifest.mjs');
  const preset = await startDeepLinkPreset({ sha: sha256(Buffer.from('other bytes')) });
  try {
    const run = await bootWithIndex(page.context(), errors, { index: preset.index, query: '?preset=signals' });
    await expect.poll(() => urlOutcome(run.messages, 0, preset.layoutUrl), { timeout: 60000 })
      .toContain('reason=fingerprint');
    expect(await datasetCount(run.page, run.messages)).toBe(0);
    expect(preset.recordingGets()).toHaveLength(1);
    expect(preset.fetchedWhileBooting(), preset.server.requests.join('\n')).toBe(true);
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// The browser build's store of downloaded recordings, and the key it files one under: the pin and the URL
// (BrowserFetch.cpp recordingCacheKey(), loading.html.in).
const RECORDING_CACHE = 'pj-recordings-v1';
const recordingCacheKey = (sha, url) => `/pj-recording-cache/${sha}?source=${encodeURIComponent(url)}`;

// The keys of the recordings the page's origin keeps, as recordingCacheKey() spells them.
async function cachedRecordings(page) {
  return page.evaluate(async (name) => {
    const requests = await (await caches.open(name)).keys();
    return requests.map(request => new URL(request.url)).map(url => url.pathname + url.search).sort();
  }, RECORDING_CACHE);
}

// Stores `body` under `key` in the page origin's recording cache, as if downloaded at `stored` (ms since the epoch)
// with `size` bytes.
async function putCachedRecording(page, key, body, { size = body.length, stored = Date.now() } = {}) {
  await page.evaluate(async ({ name, key, body, size, stored }) => {
    await (await caches.open(name)).put(key, new Response(body, {
      headers: { 'X-PJ-Size': String(size), 'X-PJ-Stored': String(stored) },
    }));
  }, { name: RECORDING_CACHE, key, body, size, stored });
}

// Boots `preset` by deep link (`query`) or, without one, picks it in the welcome dialog; returns the run once
// its layout load has ended, with that outcome.
async function loadSignals(context, errors, preset, { deepLink = true, initScript } = {}) {
  const run = await bootWithIndex(context, errors, {
    index: preset.index, query: deepLink ? '?preset=signals' : '', initScript,
  });
  if (!deepLink) {
    let dialog;
    await expect.poll(async () => {
      dialog = await presetsDialog(run.page, run.messages);
      return dialog.visible && Boolean(dialog.entries.signals?.enabled);
    }, { timeout: 30000 }).toBe(true);
    await click(run, dialog.entries.signals.open);
  }
  await expect.poll(() => urlOutcome(run.messages, 0, preset.layoutUrl), { timeout: 60000 }).not.toBe('');
  return { ...run, outcome: urlOutcome(run.messages, 0, preset.layoutUrl) };
}

// A pinned recording is kept by the browser once it has downloaded and matched its pin: every later
// visit from the same browser reads it from there, deep link or welcome dialog, without the network.
test('a pinned recording downloads once and later visits load it without the network', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const preset = await startDeepLinkPreset();
  const context = page.context();
  try {
    const first = await loadSignals(context, errors, preset);
    expect(first.outcome).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(preset.recordingGets()).toHaveLength(1);
    const key = recordingCacheKey(preset.sha, preset.recordingUrl);
    await expect.poll(() => cachedRecordings(first.page), { timeout: 30000 }).toEqual([key]);
    await first.page.close();

    for (const deepLink of [true, false]) {
      const again = await loadSignals(context, errors, preset, { deepLink });
      expect(again.outcome, `deep link ${deepLink}`).toContain('PJ_WASM_LAYOUT_LOAD_OK');
      expect(await datasetCount(again.page, again.messages)).toBe(1);
      expect(preset.recordingGets(), `deep link ${deepLink}`).toHaveLength(1);
      await again.page.close();
    }

    // A layout whose pin changed names other bytes: they download, and are kept beside the first.
    const otherSha = preset.pin(Buffer.from(fs.readFileSync(path.join(SIGNALS_FIXTURE, 'data.csv'), 'utf8')
      .replace(/0\.049787\n?$/, '0.049788\n')));
    const changed = await loadSignals(context, errors, preset, { deepLink: false });
    expect(changed.outcome).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(preset.recordingGets()).toHaveLength(2);
    await expect.poll(() => cachedRecordings(changed.page), { timeout: 30000 })
      .toEqual([key, recordingCacheKey(otherSha, preset.recordingUrl)].sort());
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// A pinned recording that spans several 8 MiB pieces and ends inside one is hashed whole: its pin matches.
test('a pinned recording larger than 8 MiB, ending mid-piece, matches its pin', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const preset = await startDeepLinkPreset();
  try {
    const rows = Array.from({ length: 400000 }, (_, row) => `${1000 + row * 0.02},0,1,1,-1,0,1\n`);
    const body = Buffer.concat([fs.readFileSync(path.join(SIGNALS_FIXTURE, 'data.csv')), Buffer.from(rows.join(''))]);
    expect(body.length).toBeGreaterThan(8 * 1024 * 1024);
    expect(body.length % (8 * 1024 * 1024)).not.toBe(0);
    preset.pin(body);
    const run = await loadSignals(page.context(), errors, preset);
    expect(run.outcome).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// A kept recording is checked against its pin like a download: one that no longer matches is refused for
// its fingerprint and dropped, so the next visit downloads it again.
test('a kept recording that no longer matches its pin is dropped, and the next visit downloads it', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const preset = await startDeepLinkPreset();
  const context = page.context();
  const key = recordingCacheKey(preset.sha, preset.recordingUrl);
  try {
    const plain = await bootWithIndex(context, errors, { index: { presets: [] } });
    await putCachedRecording(plain.page, key, 'time,value\n0,1\n');
    await plain.page.close();

    const refused = await loadSignals(context, errors, preset);
    expect(refused.outcome).toContain('reason=fingerprint');
    expect(preset.recordingGets()).toHaveLength(0);
    await expect.poll(() => cachedRecordings(refused.page), { timeout: 30000 }).toEqual([]);
    await refused.page.close();

    const recovered = await loadSignals(context, errors, preset);
    expect(recovered.outcome).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(preset.recordingGets()).toHaveLength(1);
    await expect.poll(() => cachedRecordings(recovered.page), { timeout: 30000 }).toEqual([key]);
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// Only a pinned recording is kept: nothing else can show that what the cache answers is what the layout names.
test('a recording without a pin is downloaded on every visit', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const preset = await startDeepLinkPreset({ sha: null });
  const context = page.context();
  try {
    for (const visit of [1, 2]) {
      const run = await loadSignals(context, errors, preset);
      expect(run.outcome).toContain('PJ_WASM_LAYOUT_LOAD_OK');
      expect(preset.recordingGets()).toHaveLength(visit);
      await run.page.waitForTimeout(1000);
      expect(await cachedRecordings(run.page)).toEqual([]);
      await run.page.close();
    }
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// The cache is a convenience: a browser that has none (or refuses to store, e.g. over its quota) loads the
// recording from the network as before, and nothing is kept.
test('a recording loads when the browser cannot keep it', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const preset = await startDeepLinkPreset();
  const context = page.context();
  try {
    const stubs = [
      () => { CacheStorage.prototype.open = () => Promise.reject(new DOMException('no cache', 'SecurityError')); },
      () => { Cache.prototype.put = () => Promise.reject(new DOMException('full', 'QuotaExceededError')); },
    ];
    for (const [visit, initScript] of stubs.entries()) {
      const run = await loadSignals(context, errors, preset, { initScript });
      expect(run.outcome, String(visit)).toContain('PJ_WASM_LAYOUT_LOAD_OK');
      expect(preset.recordingGets()).toHaveLength(visit + 1);
      await run.page.waitForTimeout(1000);
      await run.page.close();
    }
    const plain = await loadSignals(context, errors, preset);
    expect(plain.outcome).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(preset.recordingGets()).toHaveLength(3);
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// The kept recordings stay within 1 GiB: storing one past it first drops the longest-kept ones.
test('keeping a recording drops the longest-kept ones past the cache budget', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const preset = await startDeepLinkPreset();
  const context = page.context();
  const MiB = 1024 * 1024;
  try {
    const plain = await bootWithIndex(context, errors, { index: { presets: [] } });
    // Stand-ins that declare their sizes: together they fill the budget.
    await putCachedRecording(plain.page, recordingCacheKey('a'.repeat(64), 'https://old.example/a'), 'a',
      { size: 700 * MiB, stored: 1 });
    await putCachedRecording(plain.page, recordingCacheKey('b'.repeat(64), 'https://old.example/b'), 'b',
      { size: 324 * MiB, stored: 2 });
    await plain.page.close();

    const run = await loadSignals(context, errors, preset);
    expect(run.outcome).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expect.poll(() => cachedRecordings(run.page), { timeout: 30000 }).toEqual([
      recordingCacheKey('b'.repeat(64), 'https://old.example/b'),
      recordingCacheKey(preset.sha, preset.recordingUrl),
    ].sort());
    expect(errors).toEqual([]);
  } finally {
    await preset.server.close();
  }
});

// The deployed samples (resources/wasm-presets) in the acceptance package that
// carries them (scripts/package_wasm.mjs --presets), served like production
// from presets/ beside the page. Every other spec boots the plain package, so
// no welcome dialog sits over its window.
const SAMPLES_PACKAGE = path.resolve(
  process.env.PJ_WASM_PRESETS_PACKAGE_DIR || path.join(__dirname, '..', '..', 'build-wasm-probe', 'deploy-presets'));
const SAMPLE_DIRECTORY = path.join(__dirname, '..', '..', 'resources', 'wasm-presets');
const SAMPLE_IDS = ['builtin_filters', 'anymal', 'city_drive'];
const HOSTED_ORIGIN = require('./fixtures/hosted_assets.json').origin;
// A small CSV preset the download and robot-hosting cases serve; not a deployed sample.
const SIGNALS_FIXTURE = path.join(__dirname, 'fixtures', 'signals');

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

// The car model (one mesh, from the asset host) drawn, and the lidar scan (Cloudini-compressed)
// restored in the map frame and drawn. Only once the cursor has passed the first scan, a few tens
// of milliseconds into the clip.
const cityDriveReady = (models, dock) => models.robots.live === 1 && models.robots.placeholders === 0
  && models.robots.draws > 0 && models.robots.status.length === 1 && /\b1\/1 meshes/.test(models.robots.status[0])
  && dock.fixed === 'map' && dock.data.points.live === 1
  && dock.data.points.compressed === 1 && dock.data.points.rendered > 0;

// The City drive sample's plots, its front camera view, and the car and its lidar scan moving.
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

// The Built-in filters sample: the raw gyro yaw rate, the same signal through the built-in
// moving_average, and its built-in integral (the heading change), restored from the layout's
// <data_processors> and drawn.
const FILTERS_YAW_RATE = 'nissan/gps/duro/imu/angular_velocity/z';
async function expectFiltersViews(run) {
  await expectPlots(run, [
    [FILTERS_YAW_RATE], [`${FILTERS_YAW_RATE}[Moving Average]/value`], [`${FILTERS_YAW_RATE}[Integral]/value`],
  ]);
  const before = count(run.messages, 'PJ_WASM_FILTER_RECIPES count=');
  await run.page.evaluate(() => window.pjWasmReportFilterResultProbe());
  await expect.poll(() => count(run.messages, 'PJ_WASM_FILTER_RECIPES count=')).toBeGreaterThan(before);
  expect(latest(run.messages, 'PJ_WASM_FILTER_RECIPES count=')).toMatch(/count=2 ids=(integral,moving_average|moving_average,integral) /);
}

test('every deployed sample opens into its own views', async ({ page }) => {
  test.setTimeout(360000);
  const errors = [];
  const samples = await startSamples();
  try {
    const context = page.context();
    await page.close();
    for (const [id, expectViews] of [
      ['builtin_filters', expectFiltersViews], ['anymal', expectAnymalViews], ['city_drive', expectCityDriveViews],
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
      // replaced sample releases its own, so nothing else is left behind.
      expect(await stagedFiles(run.page, run.messages), pass).toBe(staged);
    };

    await openAnymal('load', 1);
    // Switching to another sample brings its views and replaces the one it
    // switched from; reopening ANYmal brings its robot back.
    expect(await pickSample(run, samples, 'builtin_filters')).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expectFiltersViews(run);
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

    // The sample's layout and recording, as the mirror serves them.
    const filtersLayout = fs.readFileSync(path.join(SAMPLE_DIRECTORY, 'builtin_filters', 'layout.pj4.xml'), 'utf8')
      .replaceAll(HOSTED_ORIGIN, samples.mirror.origin);
    const recordingUrl = elementAttributes(filtersLayout, 'fileInfo', 'filename')[0];
    for (const [url, body, reason] of [
      // Staged, then refused: these bytes are not the file the layout names.
      [recordingUrl, Buffer.from('not the recording'), 'fingerprint'],
      // Imported, then rolled back: the layout cannot apply.
      [samples.url('builtin_filters/layout.pj4.xml'), filtersLayout.replace(
        /(<DockArea\b[^>]*>)[\s\S]*?(<\/DockArea>)/, '$1<scene3d><layer object_type="not_a_type"/></scene3d>$2'),
      'apply-after-source-load'],
    ]) {
      // A page route wins over the context's mirror route; the recording is cross-origin.
      await run.page.route(url, route => route.fulfill({
        status: 200, body, headers: { 'Access-Control-Allow-Origin': '*' } }));
      const outcome = await pickSample(run, samples, 'builtin_filters');
      await run.page.unroute(url);
      expect(outcome, url).toContain('PJ_WASM_LAYOUT_LOAD_FAILED');
      expect(outcome, url).toContain(`reason=${reason}`);
      await dismissDialogs(run.page, run.messages);
      await expect.poll(() => stagedFiles(run.page, run.messages), { timeout: 15000 }).toBe(staged);
      expect(await datasetCount(run.page, run.messages), url).toBe(1);
      expect(comparableWorkspace(await workspaceSnapshot(run.page, run.messages)), url).toBe(workspace);
    }
    // Served its own files again, it opens in place of the open sample.
    expect(await pickSample(run, samples, 'builtin_filters')).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expectFiltersViews(run);
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
  await bootApp(page, messages, errors);
  const recordingBytes = 96 * 1024 * 1024;
  const server = await startPresetServer({
    'big/layout.pj4.xml': fs.readFileSync(path.join(SIGNALS_FIXTURE, 'layout.pj4.xml')),
    'big/data.csv': { body: Buffer.alloc(recordingBytes, '0'), headers: { 'content-length': String(recordingBytes) } },
  });
  try {
    const before = await wasmHeap(page, messages);
    expect(await openPreset(page, messages, server.url('big/layout.pj4.xml'))).toContain('reason=fingerprint');
    expect(server.requests).toEqual(['GET /presets/big/layout.pj4.xml', 'GET /presets/big/data.csv']);
    // Its progress arrived against the declared size while it downloaded, and ended whole.
    const progress = messages
      .map(message => message.match(/PJ_WASM_LAYOUT_URL_PROGRESS received=(-?\d+) total=(-?\d+)/))
      .filter(Boolean)
      .map(match => ({ received: Number(match[1]), total: Number(match[2]) }));
    expect(progress.at(-1)).toEqual({ received: recordingBytes, total: recordingBytes });
    expect(progress.some(({ received, total }) => total === recordingBytes && received < total)).toBe(true);
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
  await bootApp(page, messages, errors);
  const server = await startPresetServer({
    'empty/layout.pj4.xml': fs.readFileSync(path.join(SIGNALS_FIXTURE, 'layout.pj4.xml')),
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
