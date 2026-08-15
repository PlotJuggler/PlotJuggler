// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const { waitForQtApp } = require('./support/app');
const { clickActiveDialogButton } = require('./support/dialogs');
const { base64Fixture } = require('./support/fixtures');
const { openFileChooser } = require('./support/pickers');

async function csvDialogControlGeometry(page, consoleMessages) {
  const previous = consoleMessages.filter(
    message => message.includes('PJ_WASM_CSV_DIALOG_CONTROLS delimiter='),
  ).length;
  await page.evaluate(() => window.pjWasmReportCsvDialogControlsProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_CSV_DIALOG_CONTROLS delimiter=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previous);
  const message = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_CSV_DIALOG_CONTROLS delimiter='),
  ).at(-1) || '';
  const match = message.match(
    /delimiter=(-?\d+),(-?\d+) semicolon=(-?\d+),(-?\d+) select=(-?\d+),(-?\d+) time=(-?\d+),(-?\d+)/,
  );
  expect(match, `unparseable CSV-dialog control geometry: ${message}`).not.toBeNull();
  return {
    delimiter: { x: Number(match[1]), y: Number(match[2]) },
    semicolon: { x: Number(match[3]), y: Number(match[4]) },
    select: { x: Number(match[5]), y: Number(match[6]) },
    time: { x: Number(match[7]), y: Number(match[8]) },
  };
}

async function ingestControlGeometry(page, consoleMessages, required) {
  for (let attempt = 0; attempt < 20; ++attempt) {
    const previous = consoleMessages.filter(
      message => message.includes('PJ_WASM_INGEST_CONTROLS stop='),
    ).length;
    await page.evaluate(() => window.pjWasmReportIngestControlsProbe());
    await expect.poll(
      () => consoleMessages.filter(message => message.includes('PJ_WASM_INGEST_CONTROLS stop=')).length,
      { timeout: 5000 },
    ).toBeGreaterThan(previous);
    const message = consoleMessages.filter(
      entry => entry.includes('PJ_WASM_INGEST_CONTROLS stop='),
    ).at(-1) || '';
    const match = message.match(
      /stop=(-?\d+),(-?\d+) remove=(-?\d+),(-?\d+) keep=(-?\d+),(-?\d+) cancel=(-?\d+),(-?\d+)/,
    );
    expect(match, `unparseable ingest-control geometry: ${message}`).not.toBeNull();
    const geometry = {
      stop: { x: Number(match[1]), y: Number(match[2]) },
      remove: { x: Number(match[3]), y: Number(match[4]) },
      keep: { x: Number(match[5]), y: Number(match[6]) },
      cancel: { x: Number(match[7]), y: Number(match[8]) },
    };
    if (geometry[required].x >= 0 && geometry[required].y >= 0) {
      return geometry;
    }
    await page.waitForTimeout(100);
  }
  throw new Error(`ingest ${required} control did not become visible`);
}

// Stops the in-flight import from the loading row's own affordances. The two
// glyphs ARE the choice — ✕ ('keep') keeps what arrived, the bin ('remove')
// discards it — so this is one click and no confirmation dialog: the host acts
// on which glyph was hit rather than asking afterwards.
async function stopIngest(page, screen, consoleMessages, action) {
  const controls = await ingestControlGeometry(page, consoleMessages, action);
  await page.mouse.click(screen.x + controls[action].x, screen.y + controls[action].y);
}

function makeLargeCsv(rowCount = 1_000_000) {
  const rows = ['time,value\n'];
  for (let index = 0; index < rowCount; ++index) {
    rows.push(`${index},${index % 1000}\n`);
  }
  return Buffer.from(rows.join(''));
}

test('browser picker falls back under cross-origin isolation without removing the native API', async ({ page }) => {
  test.setTimeout(90000);
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  await expect.poll(() => page.evaluate(() => crossOriginIsolated)).toBe(true);
  await expect.poll(
    () => page.evaluate(() => typeof window.showOpenFilePicker),
  ).toBe('function');

  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'native-api-present.pjprobe',
    mimeType: 'application/octet-stream',
    buffer: Buffer.from([1, 2, 3, 4, 5]),
  });
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_INGRESS_PROBE_OK')) || '',
    { timeout: 10000 },
  ).toContain('bytes=5 sum=15');

  expect(await page.evaluate(() => typeof window.showOpenFilePicker)).toBe('function');
  expect(errors).toEqual([]);
});

test('browser file dialog can reject then accept without a nested event loop', async ({ page }) => {
  test.setTimeout(90000);
  const errors = [];
  const consoleMessages = [];
  // Qt prefers the File System Access API when Chrome exposes it. Playwright's
  // setFiles hook targets `<input type=file>`, so force Qt's documented HTML
  // fallback for this automation-only byte injection.
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);

  // Qt renders widgets into its canvas. Address the upload icon on the file
  // page relative to that canvas.
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'same-name.pjprobe',
    mimeType: 'application/octet-stream',
    buffer: Buffer.from([1, 2, 3, 4, 5]),
  });
  // The static fixture has a real plugin configuration dialog. Rejecting must
  // release FileLoader's suspended coroutine and leave the queue usable.
  // Let the dialog's finished callback unwind the suspended coroutine and let
  // Qt dispose the browser input element before invoking the picker again.
  await page.waitForTimeout(1500);
  await clickActiveDialogButton(page, screen, consoleMessages, 'cancel');
  // Wait for FileLoader's own rejection marker instead of a fixed delay, so
  // the absence check below can't race the coroutine unwind.
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_FILE_LOAD_REJECTED')),
    { timeout: 10000 },
  ).toBe(true);
  // Anchor the accept-path poll after the rejection marker: a stray early
  // PROBE_OK (there shouldn't be one, but a fixed-length find() can't tell)
  // must not satisfy it.
  const rejectionIndex = consoleMessages.findIndex(message => message.includes('PJ_FILE_LOAD_REJECTED'));
  expect(consoleMessages.some(message => message.includes('PJ_WASM_INGRESS_PROBE_OK'))).toBe(false);

  const secondChooser = await openFileChooser(page, screen, true);
  await secondChooser.setFiles({
    name: 'same-name.pjprobe',
    mimeType: 'application/octet-stream',
    buffer: Buffer.from([1, 2, 3, 4, 5]),
  });
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.slice(rejectionIndex + 1).find(message => message.includes('PJ_WASM_INGRESS_PROBE_OK')) ||
        '',
    { timeout: 10000 },
  ).toContain('bytes=5 sum=15');
  expect(errors).toEqual([]);
});

test('official CSV plugin previews and imports browser-selected data', async ({ page }) => {
  test.setTimeout(90000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await expect(page).toHaveTitle(/PlotJuggler/i);
  await waitForQtApp(page, consoleMessages);

  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'sample.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,value,temp\n0,1,10\n1,2,20\n2,3,30\n'),
  });

  // Accept the production CSV configuration through its live Qt button.
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('plugin=CSV Loader');
  const loadMessage = consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '';
  expect(loadMessage).toContain('catalog_items=3');
  expect(loadMessage).toContain('scalar_count=3');
  expect(loadMessage).toContain('scalar_series=sample/temp,sample/time,sample/value');
  expect(loadMessage).toContain('sample/value:3@0..2000000000=1..3');
  expect(consoleMessages.some(message => message.includes('Imported 3 rows, 0 skipped'))).toBe(true);
  expect(errors).toEqual([]);
});

test('official CSV plugin applies delimiter and time-column choices', async ({ page }) => {
  test.setTimeout(90000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'semicolon.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time;value;temp\n10;1;10\n20;2;20\n30;3;30\n'),
  });
  await page.waitForTimeout(750);

  // Select semicolon in the delimiter combo, then select `time` as the X axis.
  // The controls live in Qt's canvas; sample their current geometry because the
  // canonical host title bar changes the embedded plugin's origin.
  let controls = await csvDialogControlGeometry(page, consoleMessages);
  await page.mouse.click(screen.x + controls.delimiter.x, screen.y + controls.delimiter.y);
  await page.waitForTimeout(250);
  controls = await csvDialogControlGeometry(page, consoleMessages);
  expect(controls.semicolon.x).toBeGreaterThanOrEqual(0);
  expect(controls.semicolon.y).toBeGreaterThanOrEqual(0);
  await page.mouse.click(screen.x + controls.semicolon.x, screen.y + controls.semicolon.y);
  await page.waitForTimeout(500);
  controls = await csvDialogControlGeometry(page, consoleMessages);
  await page.mouse.click(screen.x + controls.select.x, screen.y + controls.select.y);
  await page.waitForTimeout(500);
  controls = await csvDialogControlGeometry(page, consoleMessages);
  expect(controls.time.x).toBeGreaterThanOrEqual(0);
  expect(controls.time.y).toBeGreaterThanOrEqual(0);
  await page.mouse.click(screen.x + controls.time.x, screen.y + controls.time.y);
  await page.waitForTimeout(500);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('plugin=CSV Loader');
  const loadMessage = consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '';
  expect(loadMessage).toContain('catalog_items=2');
  expect(loadMessage).toContain('scalar_count=2');
  expect(loadMessage).toContain('scalar_series=semicolon/temp,semicolon/value');
  expect(loadMessage).toContain('semicolon/value:3@10000000000..30000000000=1..3');
  expect(consoleMessages.some(message => message.includes('Imported 3 rows, 0 skipped'))).toBe(true);
  expect(errors).toEqual([]);
});

test('official CSV plugin can continue after a malformed-row warning', async ({ page }) => {
  test.setTimeout(90000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'malformed.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,value,temp\n0,1,10\n1,2\n2,3,30\n'),
  });
  await page.waitForTimeout(750);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  // CsvSource asks on its ingest worker while the GUI remains event-driven.
  // Wait for FileLoader to queue the real QMessageBox, then activate its
  // default Continue action by keyboard; the app-modal dialog's position can
  // change as Qt lays out the wrapped text.
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLUGIN_MESSAGE')) || '',
    { timeout: 15000 },
  ).toContain('Continue loading?');
  await page.waitForTimeout(750);
  await page.keyboard.press('Enter');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('plugin=CSV Loader');
  const loadMessage = consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '';
  expect(loadMessage).toContain('catalog_items=3');
  expect(loadMessage).toContain('scalar_count=3');
  expect(loadMessage).toContain('malformed/value:2@0..1000000000=1..3');
  expect(consoleMessages.some(message => message.includes('Imported 2 rows, 1 skipped'))).toBe(true);
  expect(errors).toEqual([]);
});

test('official CSV plugin can stop and keep a browser import', async ({ page }) => {
  test.setTimeout(120000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({ name: 'large.csv', mimeType: 'text/csv', buffer: makeLargeCsv() });
  await page.waitForTimeout(500);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await stopIngest(page, screen, consoleMessages, 'keep');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 30000 },
  ).toContain('plugin=CSV Loader');
  const loadMessage = consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '';
  const partial = loadMessage.match(/large\/value:(\d+)@/);
  expect(partial).not.toBeNull();
  expect(Number(partial[1])).toBeGreaterThan(0);
  expect(Number(partial[1])).toBeLessThanOrEqual(1_000_000);
  expect(consoleMessages.some(message => message.includes('import cancelled by user; keeping the partial load')))
    .toBe(true);
  expect(errors).toEqual([]);
});

test('official CSV plugin can discard a partial import and load again', async ({ page }) => {
  test.setTimeout(120000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({ name: 'discard.csv', mimeType: 'text/csv', buffer: makeLargeCsv() });
  await page.waitForTimeout(500);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await stopIngest(page, screen, consoleMessages, 'remove');

  await expect.poll(
    () => consoleMessages.some(message => message.includes('import discarded by user; the new dataset is removed')),
    { timeout: 30000 },
  ).toBe(true);
  expect(consoleMessages.some(message => message.includes('PJ_FILE_LOAD_OK'))).toBe(false);

  // A discarded load must release the queue and its empty dataset shell.
  const retryChooser = await openFileChooser(page, screen, true);
  await retryChooser.setFiles({
    name: 'after-discard.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,value\n0,5\n1,6\n'),
  });
  await page.waitForTimeout(750);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('after-discard/value:2@0..1000000000=5..6');
  expect(errors).toEqual([]);
});

test('same-named CSV uploads keep distinct browser identities and data', async ({ page }) => {
  test.setTimeout(90000);
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  let uploadCount = 0;
  const loadCsv = async bytes => {
    const chooser = await openFileChooser(page, screen, uploadCount > 0);
    ++uploadCount;
    await chooser.setFiles({
      name: 'same.csv',
      mimeType: 'text/csv',
      buffer: Buffer.from(bytes),
    });
    await page.waitForTimeout(750);
    await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  };

  await loadCsv('time,value\n0,1\n1,2\n');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length,
    { timeout: 15000 },
  ).toBe(1);
  await page.waitForTimeout(1500);
  await loadCsv('time,value\n0,5\n1,6\n');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length,
    { timeout: 15000 },
  ).toBe(2);

  const loads = consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK'));
  expect(loads[0]).toContain('same/value:2@0..1000000000=1..2');
  expect(loads[1]).toContain('same/value:2@0..1000000000=5..6');
  const identities = loads.map(message => message.match(/identity=([^ ]+)/)?.[1]);
  expect(identities[0]).toBeTruthy();
  expect(identities[1]).toBeTruthy();
  expect(identities[0]).not.toBe(identities[1]);
  expect(errors).toEqual([]);
});

test('official Protobuf parser imports scalars and cold-decodes a Foxglove object', async ({ page }) => {
  test.setTimeout(120000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'protobuf-foxglove.mcap',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('protobuf_telemetry_foxglove_tf.mcap.b64'),
  });
  await page.waitForTimeout(1500);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('/protobuf/telemetry/temperature:3@')) || '',
    { timeout: 30000 },
  ).toContain('=12.5..-2');
  const object = consoleMessages.find(
    message => message.includes('PJ_WASM_PROTOBUF_OBJECT_OK topic=/foxglove/tf'),
  ) || '';
  expect(object).toContain('entries=1 type=kFrameTransforms');
  expect(object).toContain('transforms=1 parent=map child=base_link');
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PROTOBUF_OBJECT_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('official Protobuf parser reports a bad descriptor and leaves the load queue reusable', async ({ page }) => {
  test.setTimeout(120000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'bad-protobuf.mcap',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('protobuf_bad_schema.mcap.b64'),
  });
  await page.waitForTimeout(1500);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLUGIN_MESSAGE title=Parser Error')) || '',
    { timeout: 15000 },
  ).toContain('failed to parse FileDescriptorSet');
  // QMessageBox geometry depends on the wrapped diagnostic text. Its default
  // OK action is keyboard-stable even when the button's canvas coordinate is
  // not. The diagnostic is emitted just before the queued GUI presentation, so
  // wait for the modal window to acquire focus before sending Enter.
  await page.waitForTimeout(750);
  await page.keyboard.press('Enter');
  await page.waitForTimeout(1500);

  // A failed MCAP import must release the suspended worker and queue. Prove it
  // with a real CSV import rather than only checking that the error appeared.
  const retryChooser = await openFileChooser(page, screen, true);
  await retryChooser.setFiles({
    name: 'after-mcap-error.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,value\n0,7\n1,8\n'),
  });
  await page.waitForTimeout(750);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('after-mcap-error/value:2@')) || '',
    { timeout: 15000 },
  ).toContain('=7..8');
  expect(errors).toEqual([]);
});
