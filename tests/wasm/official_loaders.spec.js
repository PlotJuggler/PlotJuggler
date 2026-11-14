// SPDX-License-Identifier: MPL-2.0
// The official file loaders built into the wasm app import browser-selected
// files: Parquet, Arrow IPC (inside MCAP), and the CAN logs (candump with a DBC
// picked from its own dialog, BLF, MF4). Fixtures: fixtures/generate_official_loader_fixtures.py
// (Parquet, MF4, Arrow MCAP); sample.blf is pj-official-plugins' data_load_blf/test_data/sample.blf.
const fs = require('fs');
const { test, expect } = require('@playwright/test');
const { waitForQtApp } = require('./support/app');
const { clickActiveDialogButton } = require('./support/dialogs');
const { base64Fixture } = require('./support/fixtures');
const { openFileChooser, retryChooser } = require('./support/pickers');

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

async function boot(page, consoleMessages, errors) {
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return screen;
}

// Named, visible buttons of the active dialog, canvas-relative.
async function dialogWidgets(page, consoleMessages) {
  const marker = 'PJ_WASM_DIALOG_WIDGETS ';
  const previous = consoleMessages.filter(message => message.includes(marker)).length;
  await page.evaluate(() => window.pjWasmReportDialogWidgetsProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes(marker)).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previous);
  const message = consoleMessages.filter(entry => entry.includes(marker)).at(-1);
  const widgets = {};
  for (const entry of message.slice(message.indexOf(marker) + marker.length).split(';')) {
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
  const widgets = await dialogWidgets(page, consoleMessages);
  expect(widgets[button], `dialog has no ${button}`).toBeDefined();
  const dbc = await retryChooser(page, () => page.mouse.click(screen.x + widgets[button].x, screen.y + widgets[button].y));
  await dbc.setFiles({ name: 'engine.dbc', mimeType: 'text/plain', buffer: Buffer.from(SAMPLE_DBC) });
  await page.waitForTimeout(1000);
}

// Picks one file, optionally acts on the plugin's dialog, accepts it, and
// returns the probe's PJ_FILE_LOAD_OK line (a failure shows the loader's log).
async function importFile(page, screen, consoleMessages, file, inDialog = async () => {}) {
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles(file);
  await page.waitForTimeout(1500);
  await inDialog();
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  // The probe records the outcome in the page too: Firefox can drop the console line.
  const outcome = async () => (await page.evaluate(() => (globalThis.pjWasmLoadOutcomes || [])[0])) || '';
  let line = '';
  try {
    await expect.poll(async () => (line = await outcome()), { timeout: 30000 }).not.toBe('');
  } catch (error) {
    const log = consoleMessages.slice(-40).join('\n');
    throw new Error(`${error.message}\nLast app log lines:\n${log}`);
  }
  expect(line).toContain('PJ_FILE_LOAD_OK');
  return line;
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
        widgets = await dialogWidgets(page, consoleMessages);
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
