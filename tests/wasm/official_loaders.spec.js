// SPDX-License-Identifier: MPL-2.0
// The official file loaders built into the wasm app import browser-selected
// files: ULog, Parquet, the Arrow IPC, InfluxDB and NavMap parsers (inside
// MCAP), and the CAN logs (candump with a DBC
// picked from its own dialog, BLF, MF4). Fixtures: fixtures/generate_official_loader_fixtures.py
// (Parquet, MF4, Arrow MCAP); sample.blf is pj-official-plugins' data_load_blf/test_data/sample.blf.
const crypto = require('crypto');
const fs = require('fs');
const { test, expect } = require('@playwright/test');
const { bootApp, replaceRuntimeTarget } = require('./support/app');
const { loadOutcomes } = require('./support/load_outcomes');
const { activeDialogButtonGeometry, clickActiveDialogButton } = require('./support/dialogs');
const { base64Fixture } = require('./support/fixtures');
const { elementAttributes } = require('./support/layout_xml');
const { openFileChooser, openLayoutChooser, openSourceReplayChooser, retryChooser } = require('./support/pickers');
const { openPreset, startPresetServer } = require('./support/preset_server');
const { downloadSourceLayoutFromFileMenu, sourceLayoutDecisionGeometry } = require('./support/source_layout');

const CANDUMP_LOG = [
  '(1700000000.123456) can0 100#E803',
  '(1700000000.133456) can0 100#D007',
  '(1700000000.143456) can0 100#B80B',
  '',
].join('\n');

const SAMPLE_DBC = [
  'VERSION "1.0.0"',
  '',
  'NS_ :',
  '',
  'BS_:',
  '',
  'BU_: ECU',
  '',
  'BO_ 256 EngineData: 2 ECU',
  ' SG_ Speed : 0|16@1+ (0.1,0) [0|6553.5] "km/h" ECU',
  '',
].join('\n');

// The smallest ULog the official loader accepts: header, flag bits, one format,
// one subscription and one data message per sample (`timestamp` is in µs).
function makeULog(samples) {
  const message = (type, body) => {
    const header = Buffer.alloc(3);
    header.writeUInt16LE(body.length, 0);
    header.write(type, 2, 'latin1');
    return Buffer.concat([header, body]);
  };
  const fileHeader = Buffer.from([0x55, 0x4c, 0x6f, 0x67, 0x01, 0x12, 0x35, 0x01, 0, 0, 0, 0, 0, 0, 0, 0]);
  const parts = [
    fileHeader,
    message('B', Buffer.alloc(40)),
    message('F', Buffer.from('sensor:uint64_t timestamp;float value;', 'latin1')),
    message('A', Buffer.concat([Buffer.from([0, 0, 0]), Buffer.from('sensor', 'latin1')])),
  ];
  for (const [timestampUs, value] of samples) {
    const data = Buffer.alloc(14);
    data.writeUInt16LE(0, 0);
    data.writeBigUInt64LE(BigInt(timestampUs), 2);
    data.writeFloatLE(value, 10);
    parts.push(message('D', data));
  }
  return Buffer.concat(parts);
}

async function boot(page, consoleMessages, errors) {
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  return (await bootApp(page, consoleMessages, errors)).screen;
}

// Named, visible buttons of the active dialog, canvas-relative. The probe runs
// synchronously and also leaves its report on the page (Firefox can drop the
// console line).
async function dialogWidgets(page) {
  const report = await page.evaluate(() => {
    window.pjWasmReportDialogWidgetsProbe();
    return globalThis.pjWasmDialogWidgets || '';
  });
  const widgets = {};
  for (const entry of report.split(';')) {
    const match = entry.match(/^(.+)=(-?\d+),(-?\d+)$/);
    if (match) {
      widgets[match[1]] = { x: Number(match[2]), y: Number(match[3]) };
    }
  }
  return widgets;
}

// Picks SAMPLE_DBC through a loader dialog's own file picker. The import that
// runs after the dialog closes reads it, so the pick must outlive the dialog.
async function pickDictionary(page, screen, consoleMessages, button) {
  const widgets = await dialogWidgets(page);
  expect(widgets[button], `dialog has no ${button}`).toBeDefined();
  const dbc = await retryChooser(page, () => page.mouse.click(screen.x + widgets[button].x, screen.y + widgets[button].y));
  await dbc.setFiles({ name: 'engine.dbc', mimeType: 'text/plain', buffer: Buffer.from(SAMPLE_DBC) });
  await page.waitForTimeout(1000);
}

// Picks one file, optionally acts on the plugin's dialog, accepts it, and returns
// the page's nth (0-based) load outcome, expecting PJ_FILE_LOAD_OK (a failure
// shows the loader's log).
async function importFile(page, screen, consoleMessages, file, inDialog = async () => {}, nth = 0) {
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles(file);
  await page.waitForTimeout(1500);
  // A loader in a side module (PJ_WASM_ARROW_SIDE_MODULE) opens its dialog once the module has downloaded.
  await expect.poll(async () => (await activeDialogButtonGeometry(page)).ok.x >= 0,
    { timeout: 30000 }).toBe(true);
  await inDialog();
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  return loadOkLine(page, consoleMessages, nth);
}

// Waits for the page's nth (0-based) load outcome and returns it, expecting
// PJ_FILE_LOAD_OK (a timeout shows the loader's log). The probe records the
// outcome in the page too: Firefox can drop the console line.
async function loadOkLine(page, consoleMessages, nth = 0) {
  let line = '';
  try {
    await expect.poll(async () => (line = (await loadOutcomes(page, 'PJ_FILE_LOAD_'))[nth] || ''),
      { timeout: 30000 }).not.toBe('');
  } catch (error) {
    const log = consoleMessages.slice(-40).join('\n');
    throw new Error(`${error.message}\nLast app log lines:\n${log}`);
  }
  expect(line).toContain('PJ_FILE_LOAD_OK');
  return line;
}

function parquetFile(name) {
  return { name, mimeType: 'application/octet-stream', buffer: base64Fixture('sample.parquet.b64') };
}

// The samples of one series: count, time span (ns), first and last value. A
// missing series shows the loader's log.
function seriesSamples(outcome, suffix, consoleMessages = []) {
  const list = outcome.match(/series_samples=(\S*)/)?.[1] || '';
  for (const entry of list.split(',')) {
    const match = entry.match(/^(.+):(\d+)@(-?\d+)\.\.(-?\d+)=(.+)\.\.(.+)$/);
    if (match && match[1].endsWith(suffix)) {
      return {
        name: match[1],
        count: Number(match[2]),
        span: BigInt(match[4]) - BigInt(match[3]),
        start: BigInt(match[3]),
        first: Number(match[5]),
        last: Number(match[6]),
      };
    }
  }
  const log = consoleMessages.filter(message => message.includes('pj.')).slice(-30).join('\n');
  throw new Error(`no series ending in ${suffix} in: ${list}\nLast app log lines:\n${log}`);
}

test('official ULog loader imports a browser-selected log', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'flight.ulg',
    mimeType: 'application/octet-stream',
    buffer: makeULog([[1_000_000, 1.5], [2_000_000, 2.5], [3_000_000, 4.5]]),
  });
  expect(outcome).toContain('series_samples=sensor/value:3@1000000000..3000000000=1.5..4.5');
  expect(errors).toEqual([]);
});

test('official InfluxDB parser decodes line protocol in an MCAP channel', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'influx.mcap',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('influx.mcap.b64'),
  });
  // Field series are measurement/tags/field; each line carries its own time.
  const value = seriesSamples(outcome, '/telegraf/speed/host=car/value', consoleMessages);
  expect(value).toMatchObject({ count: 3, start: 1_000_000_000n, first: 1.5, last: 4.5 });
  expect(value.span).toBe(2_000_000_000n);
  expect(errors).toEqual([]);
});

test('official NavMap parser decodes a NavMap message in an MCAP channel', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'navmap.mcap',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('navmap.mcap.b64'),
  });
  // The parser's summary series (not the generic flattening), plus the mesh
  // object topic the 3D view draws: 12 catalog items, 11 of them scalar.
  expect(outcome).toContain('catalog_items=12 scalar_count=11');
  const count = suffix => seriesSamples(outcome, suffix, consoleMessages);
  expect(count('/navmap/num_vertices')).toMatchObject({ count: 1, first: 7 });
  expect(count('/navmap/num_navcels')).toMatchObject({ count: 1, first: 5 });
  expect(count('/navmap/num_surfaces')).toMatchObject({ count: 1, first: 2 });
  expect(count('/navmap/dropped_links')).toMatchObject({ count: 1, first: 0 });
  expect(errors).toEqual([]);
});

test('official Parquet loader imports a browser-selected file', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'sample.parquet',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('sample.parquet.b64'),
  });
  const value = seriesSamples(outcome, '/value', consoleMessages);
  expect(value).toMatchObject({ count: 3, first: 1.5, last: 4.5 });
  expect(value.span).toBe(2_000_000_000n);
  expect(errors).toEqual([]);
});

// Whether the served package ships the Arrow family as the side module plugins/arrow.wasm
// (PJ_WASM_ARROW_SIDE_MODULE).
async function hasArrowSideModule(request) {
  const base = process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html';
  const manifest = await request.get(new URL('manifest.json', base).href).then(r => r.json()).catch(() => null);
  return Boolean(manifest?.files?.some(file => file.path.endsWith('/plugins/arrow.wasm')));
}

// The URLs of every arrow.wasm request the page's context makes, as they happen. Chromium reports the worker's
// request; Firefox does not, so a count can only be bounded above.
function recordSideModuleFetches(page) {
  const fetches = [];
  page.context().on('request', req => {
    if (req.url().includes('/plugins/arrow.wasm')) {
      fetches.push(req.url());
    }
  });
  return fetches;
}

// With PJ_WASM_ARROW_SIDE_MODULE the Arrow plugins are the side module plugins/arrow.wasm: neither the boot nor
// another import fetches it, the first Parquet import does, once. Skipped for a package built without it.
test('the Arrow side module is fetched by the first Parquet import only', async ({ page, request, browserName }) => {
  test.setTimeout(120000);
  test.skip(!await hasArrowSideModule(request), 'the package has no Arrow side module');
  const fetches = recordSideModuleFetches(page);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  await importFile(page, screen, consoleMessages, {
    name: 'speeds.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,speed\n1,1.5\n2,2.5\n3,4.5\n'),
  });
  expect(await page.evaluate(() => globalThis.pjWasmSideModule)).toBeUndefined();
  expect(fetches).toEqual([]);

  const picked = Date.now();
  const outcome = await importFile(page, screen, consoleMessages, parquetFile('first.parquet'), undefined, 1);
  const firstMs = Date.now() - picked;
  const loaded = await page.evaluate(() => globalThis.pjWasmSideModule);
  expect(loaded).toMatch(/bytes=\d+ fetch_ms=\d+ link_ms=\d+/);
  expect(seriesSamples(outcome, '/value', consoleMessages)).toMatchObject({ count: 3, first: 1.5, last: 4.5 });
  // A second import reuses the linked module: no second download, no second link.
  const again = Date.now();
  await importFile(page, screen, consoleMessages, parquetFile('second.parquet'), undefined, 2);
  const secondMs = Date.now() - again;
  expect(await page.evaluate(() => globalThis.pjWasmSideModule)).toBe(loaded);
  test.info().annotations.push({ type: 'side-module', description: `${loaded} first_import_ms=${firstMs} second_import_ms=${secondMs}` });
  console.log(`PJ_TEST_SIDE_MODULE ${loaded} first_import_ms=${firstMs} second_import_ms=${secondMs} fetches=${fetches.length}`);
  if (browserName === 'chromium') {
    expect(fetches).toHaveLength(1);
  } else {
    // Firefox does not report a worker's requests to the context: only an upper bound is observable.
    expect(fetches.length).toBeLessThanOrEqual(1);
  }
  expect(errors).toEqual([]);
});

// A failed download is not remembered: the next Parquet import downloads the module again and loads.
test('a failed Arrow side module download is retried by the next Parquet import', async ({ page, request }) => {
  test.setTimeout(120000);
  test.skip(!await hasArrowSideModule(request), 'the package has no Arrow side module');
  let refused = 0;
  await page.context().route('**/plugins/arrow.wasm', route => {
    if (refused === 0) {
      refused += 1;
      return route.fulfill({ status: 503, body: 'unavailable' });
    }
    return route.continue();
  });
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles(parquetFile('first.parquet'));
  // Ends on the failure, or on the loader's dialog where the refusal never happened.
  let failed = '';
  await expect.poll(async () => (failed = (await loadOutcomes(page, 'PJ_FILE_LOAD_'))[0] || '') !== ''
    || (await activeDialogButtonGeometry(page)).ok.x >= 0, { timeout: 30000 }).toBe(true);
  // Firefox does not route a worker's requests: the download then succeeds and there is nothing to retry.
  test.skip(refused === 0, 'this browser does not route the worker\'s request');
  expect(failed).toContain('PJ_FILE_LOAD_FAILED');
  expect(failed).toContain('HTTP 503');
  // Dismiss the failure's warning, if the app shows one.
  await page.waitForTimeout(500);
  if ((await activeDialogButtonGeometry(page)).ok.x >= 0) {
    await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  }

  const outcome = await importFile(page, screen, consoleMessages, parquetFile('second.parquet'), undefined, 1);
  expect(seriesSamples(outcome, '/value', consoleMessages)).toMatchObject({ count: 3, first: 1.5, last: 4.5 });
  expect(await page.evaluate(() => globalThis.pjWasmSideModule)).toMatch(/bytes=\d+/);
});

test('official Arrow IPC parser decodes an MCAP channel', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'arrow.mcap',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('arrow.mcap.b64'),
  });
  expect(outcome).toContain('series_samples=');
  const value = seriesSamples(outcome, '/sensor/value', consoleMessages);
  expect(value).toMatchObject({ count: 3, start: 1_000_000_000n, first: 1.5, last: 4.5 });
  expect(value.span).toBe(2_000_000_000n);
  expect(errors).toEqual([]);
});

test('official candump loader decodes with a DBC picked in its dialog', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'capture.log',
    mimeType: 'text/plain',
    buffer: Buffer.from(CANDUMP_LOG),
  }, () => pickDictionary(page, screen, consoleMessages, 'buttonDictionary'));
  const speed = seriesSamples(outcome, '/Speed', consoleMessages);
  expect(speed).toMatchObject({ count: 3, first: 100, last: 300 });
  expect(speed.span).toBe(20_000_000n);
  expect(errors).toEqual([]);
});

// The DBC picked in the loader's dialog is a side file of the source: the
// source-bound layout names it and its fingerprint like the data file, and a
// replay in a fresh page asks for both before the import decodes as before;
// opened as a preset, it downloads both.
test('a source layout saves the DBC picked in the candump dialog and replays it after both are reselected', async ({ page }) => {
  test.setTimeout(180000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const capture = { name: 'capture.log', mimeType: 'text/plain', buffer: Buffer.from(CANDUMP_LOG) };
  const dictionary = { name: 'engine.dbc', mimeType: 'text/plain', buffer: Buffer.from(SAMPLE_DBC) };
  await importFile(page, screen, consoleMessages, capture,
    () => pickDictionary(page, screen, consoleMessages, 'buttonDictionary'));

  const downloaded = await downloadSourceLayoutFromFileMenu(page, screen, consoleMessages);
  const xml = downloaded.bytes.toString('utf8');
  expect(xml).not.toContain('/pj_uploads');
  expect(xml).not.toContain('pj-upload://');
  expect(elementAttributes(downloaded.bytes, 'fileInfo', 'filename')).toEqual(['capture.log']);
  expect(elementAttributes(downloaded.bytes, 'sideFile', 'filename')).toEqual(['engine.dbc']);
  expect(elementAttributes(downloaded.bytes, 'sideFile', 'content_sha256')).toEqual([
    crypto.createHash('sha256').update(dictionary.buffer).digest('hex'),
  ]);
  expect(xml).toContain('"engine.dbc"');

  const allMessages = [];
  const replay = await replaceRuntimeTarget(page, allMessages, errors, { hideOpenFilePicker: true });
  const layoutChooser = await openLayoutChooser(replay.page);
  await layoutChooser.setFiles({ name: downloaded.fileName, mimeType: 'application/xml', buffer: downloaded.bytes });
  await expect.poll(
    () => replay.consoleMessages.some(message => message.includes('PJ_WASM_SOURCE_LAYOUT_DECISION')),
    { timeout: 15000 },
  ).toBe(true);
  let geometry = await sourceLayoutDecisionGeometry(replay.page, replay.consoleMessages);
  const dataChooser = await openSourceReplayChooser(replay.page, replay.screen, geometry, replay.consoleMessages);
  await dataChooser.setFiles(capture);
  await expect.poll(
    () => replay.consoleMessages.some(message => message.includes('PJ_WASM_SOURCE_LAYOUT_STAGED index=1')),
    { timeout: 15000 },
  ).toBe(true);
  // The same decision now asks for the dictionary; nothing is imported yet.
  geometry = await sourceLayoutDecisionGeometry(replay.page, replay.consoleMessages);
  const dbcChooser = await openSourceReplayChooser(replay.page, replay.screen, geometry, replay.consoleMessages);
  await dbcChooser.setFiles(dictionary);

  const line = await loadOkLine(replay.page, replay.consoleMessages);
  const speed = seriesSamples(line, '/Speed', replay.consoleMessages);
  expect(speed).toMatchObject({ count: 3, first: 100, last: 300 });

  // Opened by URL (a preset), the same layout downloads the dictionary from
  // beside its data file, with no picker.
  const server = await startPresetServer({
    'can/layout.pj4.xml': downloaded.bytes,
    'can/capture.log': capture.buffer,
    'can/engine.dbc': dictionary.buffer,
  });
  try {
    const preset = await replaceRuntimeTarget(replay.page, [], errors);
    expect(await openPreset(preset.page, preset.consoleMessages, server.url('can/layout.pj4.xml')))
      .toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(server.requests).toEqual([
      'GET /presets/can/layout.pj4.xml', 'GET /presets/can/capture.log', 'GET /presets/can/engine.dbc']);
    const presetLine = await loadOkLine(preset.page, preset.consoleMessages);
    expect(seriesSamples(presetLine, '/Speed', preset.consoleMessages)).toMatchObject({ count: 3, first: 100, last: 300 });
  } finally {
    await server.close();
  }
  expect(errors).toEqual([]);
});

test('official BLF loader decodes a channel with a DBC picked in its dialog', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'sample.blf',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('sample.blf.b64'),
  }, () => pickDictionary(page, screen, consoleMessages, 'buttonDbcCh1'));
  // Channel 1 carries 0x100 twice, 10 ms apart: E8 03 then D0 07 (Speed 100 then 200 km/h).
  const speed = seriesSamples(outcome, '/Speed', consoleMessages);
  expect(speed).toMatchObject({ count: 2, first: 100, last: 200 });
  expect(speed.span).toBe(10_000_000n);
  expect(errors).toEqual([]);
});

test('official MF4 loader imports a browser-selected measurement', async ({ page }) => {
  test.setTimeout(90000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  const outcome = await importFile(page, screen, consoleMessages, {
    name: 'sample.mf4',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('sample.mf4.b64'),
  });
  // MF4 time is absolute (measurement start + offset): assert the 2 s span only.
  const value = seriesSamples(outcome, '/value', consoleMessages);
  expect(value).toMatchObject({ count: 3, first: 1.5, last: 4.5 });
  expect(value.span).toBe(2_000_000_000n);
  expect(errors).toEqual([]);
});

// The Data Exporter writes its file inside the browser's sandbox and the app
// hands it to the browser as a download, once per format.
test('official Data Exporter downloads CSV and Parquet exports', async ({ page }) => {
  test.setTimeout(120000);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  await importFile(page, screen, consoleMessages, {
    name: 'speeds.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,speed\n1,1.5\n2,2.5\n3,4.5\n'),
  });

  // A successful export closes the toolbox, so each format opens it afresh.
  const exportAs = async (format) => {
    await page.evaluate(() => window.pjWasmOpenDataExporterProbe());
    let widgets;
    try {
      await expect.poll(async () => {
        widgets = await dialogWidgets(page);
        return Boolean(widgets.buttonAddAllFiles && widgets.saveButton && widgets[format]);
      }, { timeout: 15000 }).toBe(true);
    } catch (error) {
      throw new Error(`${error.message}\nexporter controls: ${JSON.stringify(widgets)}`);
    }
    await page.mouse.click(screen.x + widgets.buttonAddAllFiles.x, screen.y + widgets.buttonAddAllFiles.y);
    await page.mouse.click(screen.x + widgets[format].x, screen.y + widgets[format].y);
    await page.waitForTimeout(1000);
    const download = page.waitForEvent('download', { timeout: 30000 });
    await page.mouse.click(screen.x + widgets.saveButton.x, screen.y + widgets.saveButton.y);
    const file = await download;
    return { name: file.suggestedFilename(), bytes: fs.readFileSync(await file.path()) };
  };

  const csv = await exportAs('csvButton');
  expect(csv.name).toMatch(/\.csv$/);
  const lines = csv.bytes.toString('utf8').trim().split('\n');
  const column = lines[0].split(',').indexOf('speeds/speed');
  expect(column, `no speeds/speed column in: ${lines[0]}`).toBeGreaterThan(0);
  expect(lines.slice(1).map(line => Number(line.split(',')[column]))).toEqual([1.5, 2.5, 4.5]);

  const parquet = await exportAs('parquetButton');
  expect(parquet.name).toMatch(/\.parquet$/);
  expect(parquet.bytes.subarray(0, 4).toString('latin1')).toBe('PAR1');
  expect(parquet.bytes.subarray(-4).toString('latin1')).toBe('PAR1');
  expect(errors).toEqual([]);
});

// Opening the Data Exporter links the Arrow side module whatever format is picked later: a session that imported
// no Parquet file gets a usable exporter, Parquet included, and opening it again downloads nothing.
test('opening the Data Exporter loads the Arrow side module once', async ({ page, request }) => {
  test.setTimeout(120000);
  test.skip(!await hasArrowSideModule(request), 'the package has no Arrow side module');
  const fetches = recordSideModuleFetches(page);
  const consoleMessages = [];
  const errors = [];
  const screen = await boot(page, consoleMessages, errors);
  await importFile(page, screen, consoleMessages, {
    name: 'speeds.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,speed\n1,1.5\n2,2.5\n3,4.5\n'),
  });
  expect(await page.evaluate(() => globalThis.pjWasmSideModule)).toBeUndefined();
  expect(fetches).toEqual([]);

  const openExporter = async () => {
    await page.evaluate(() => window.pjWasmOpenDataExporterProbe());
    let widgets;
    try {
      await expect.poll(async () => {
        widgets = await dialogWidgets(page);
        return Boolean(widgets.buttonAddAllFiles && widgets.saveButton && widgets.parquetButton);
      }, { timeout: 30000 }).toBe(true);
    } catch (error) {
      throw new Error(`${error.message}\nexporter controls: ${JSON.stringify(widgets)}`);
    }
    return widgets;
  };

  // First open: the module is linked, and a Parquet export works end to end (the export closes the toolbox).
  const widgets = await openExporter();
  const loaded = await page.evaluate(() => globalThis.pjWasmSideModule);
  expect(loaded).toMatch(/bytes=\d+ fetch_ms=\d+ link_ms=\d+/);
  const fetchesAfterFirstOpen = fetches.length;
  expect(fetchesAfterFirstOpen).toBeLessThanOrEqual(1);
  await page.mouse.click(screen.x + widgets.buttonAddAllFiles.x, screen.y + widgets.buttonAddAllFiles.y);
  await page.mouse.click(screen.x + widgets.parquetButton.x, screen.y + widgets.parquetButton.y);
  await page.waitForTimeout(1000);
  const download = page.waitForEvent('download', { timeout: 30000 });
  await page.mouse.click(screen.x + widgets.saveButton.x, screen.y + widgets.saveButton.y);
  const file = await download;
  expect(file.suggestedFilename()).toMatch(/\.parquet$/);
  const bytes = fs.readFileSync(await file.path());
  expect(bytes.subarray(0, 4).toString('latin1')).toBe('PAR1');
  expect(bytes.subarray(-4).toString('latin1')).toBe('PAR1');

  // Second open: the linked module serves it, nothing is downloaded or linked again.
  await openExporter();
  expect(fetches.length).toBe(fetchesAfterFirstOpen);
  expect(await page.evaluate(() => globalThis.pjWasmSideModule)).toBe(loaded);
  console.log(`PJ_TEST_SIDE_MODULE_EXPORTER ${loaded} fetches=${fetches.length}`);
  expect(errors).toEqual([]);
});
