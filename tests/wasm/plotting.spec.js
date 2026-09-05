// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const crypto = require('crypto');
const { waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { clickActiveDialogButton } = require('./support/dialogs');
const { base64Fixture } = require('./support/fixtures');
const { openFileChooser } = require('./support/pickers');
const { requestPlotState, requestMultiPlotState } = require('./support/plot_probes');
const { curveRowCenter, placeholderIconCenter } = require('./support/geometry');

function unchunkedMcapFixture() {
  return base64Fixture('unchunked_float64.mcap.b64');
}

function makeExtremaCsv(rowCount = 6001) {
  const rows = ['time,temp\n'];
  const spikeIndex = Math.floor(rowCount / 2);
  for (let index = 0; index < rowCount; ++index) {
    const time = (index / 10000).toFixed(4);
    rows.push(`${time},${index === spikeIndex ? 100 : 0}\n`);
  }
  return Buffer.from(rows.join(''));
}

function makeDenseDashCsv(rowCount = 20_001) {
  const rows = ['time,temp\n'];
  for (let index = 0; index < rowCount; ++index) {
    rows.push(`${index},${index % 2 === 0 ? -100 : 100}\n`);
  }
  return Buffer.from(rows.join(''));
}

async function seekQtPlot(page, canvas, fromRatio, toRatio) {
  const y = canvas.y + (0.5 * canvas.height);
  await page.keyboard.down('Shift');
  await page.mouse.move(canvas.x + (fromRatio * canvas.width), y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(canvas.x + (toRatio * canvas.width), y, { steps: 16 });
  await page.mouse.up({ button: 'left' });
  await page.keyboard.up('Shift');
  // PlaybackEngine updates immediately, while the visible tracker fan-out is
  // intentionally coalesced to about 30 Hz. Let its trailing edge render before
  // asking the observation-only probe for a framebuffer readback.
  await page.waitForTimeout(150);
}

async function requestCurveStyleControls(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportCurveStyleControlsProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_CURVE_STYLE_CONTROLS open=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages
    .filter(message => message.includes('PJ_WASM_CURVE_STYLE_CONTROLS open='))
    .at(-1) || '';
  const match = message.match(
    /open=(\d+) panel=(\d+),(\d+) grid=(\d+),(\d+) lines=(\d+),(\d+) dots=(\d+),(\d+) lines_dots=(\d+),(\d+) sticks=(\d+),(\d+) steps=(\d+),(\d+) steps_inverted=(\d+),(\d+) width10=(\d+),(\d+) width15=(\d+),(\d+) width20=(\d+),(\d+) width30=(\d+),(\d+)/,
  );
  expect(match, `unparseable curve-style controls: ${message}`).not.toBeNull();
  const point = index => ({ x: Number(match[index]), y: Number(match[index + 1]) });
  return {
    open: match[1] === '1',
    panel: point(2),
    grid: point(4),
    styles: [point(6), point(8), point(10), point(12), point(14), point(16)],
    widths: [point(18), point(20), point(22), point(24)],
  };
}

async function requestPlotStylePixels(consoleMessages, previousCount) {
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STYLE_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STYLE_PIXELS')).at(-1) || '';
  const match = message.match(
    /style=(\d+) width=(\d+) pixels=(\d+) bounds=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) hash=(\d+) framebuffer=(\d+)x(\d+)/,
  );
  expect(match, `unparseable plot-style readback: ${message}`).not.toBeNull();
  return {
    style: Number(match[1]),
    width: Number(match[2]),
    pixels: Number(match[3]),
    bounds: { minX: Number(match[4]), minY: Number(match[5]), maxX: Number(match[6]), maxY: Number(match[7]) },
    hash: match[8],
    framebuffer: { width: Number(match[9]), height: Number(match[10]) },
  };
}

async function requestPlotGridPixels(consoleMessages, previousCount) {
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_GRID_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_GRID_PIXELS')).at(-1) || '';
  const match = message.match(
    /major_pixels=(\d+) major_column=(-?\d+) major_period=(-?\d+) major_period_matches=(\d+) major_period_comparisons=(\d+) major_phase_mask=(\d+) major_on_phases=(\d+) minor_pixels=(\d+) minor_column=(-?\d+) minor_period=(-?\d+) minor_period_matches=(\d+) minor_period_comparisons=(\d+) minor_phase_mask=(\d+) minor_on_phases=(\d+)/,
  );
  expect(match, `unparseable plot-grid readback: ${message}`).not.toBeNull();
  return {
    major: {
      pixels: Number(match[1]), column: Number(match[2]), period: Number(match[3]),
      periodMatches: Number(match[4]), periodComparisons: Number(match[5]),
      phaseMask: Number(match[6]), onPhases: Number(match[7]),
    },
    minor: {
      pixels: Number(match[8]), column: Number(match[9]), period: Number(match[10]),
      periodMatches: Number(match[11]), periodComparisons: Number(match[12]),
      phaseMask: Number(match[13]), onPhases: Number(match[14]),
    },
  };
}

async function requestXyDialog(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportXyDialogProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_XY_DIALOG x=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_XY_DIALOG x=')).at(-1) || '';
  const match = message.match(
    /x=([^ ]*) y=([^ ]*) alias=([^ ]*) ok=(\d+),(\d+) alias_edit=(\d+),(\d+) swap=(\d+),(\d+) rect=(\d+),(\d+),(\d+)x(\d+)/,
  );
  expect(match).not.toBeNull();
  return {
    x: decodeURIComponent(match[1]),
    y: decodeURIComponent(match[2]),
    alias: decodeURIComponent(match[3]),
    ok: { x: Number(match[4]), y: Number(match[5]) },
    aliasEdit: { x: Number(match[6]), y: Number(match[7]) },
    swap: { x: Number(match[8]), y: Number(match[9]) },
    rect: {
      x: Number(match[10]),
      y: Number(match[11]),
      width: Number(match[12]),
      height: Number(match[13]),
    },
  };
}

async function requestXyDuplicateWarning(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportXyDuplicateWarningProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_XY_WARNING title=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_XY_WARNING title=')).at(-1) || '';
  const match = message.match(/title=([^ ]*) body=([^ ]*) ok=(\d+),(\d+)/);
  expect(match).not.toBeNull();
  return {
    title: decodeURIComponent(match[1]),
    body: decodeURIComponent(match[2]),
    ok: { x: Number(match[3]), y: Number(match[4]) },
  };
}

async function requestTrackerReadback(consoleMessages, previousCount) {
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TRACKER')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TRACKER')).at(-1) || '';
  const match = message.match(
    /color=([^ ]+) pixels=(\d+) bounds=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) framebuffer=(\d+)x(\d+)/,
  );
  expect(match).not.toBeNull();
  return {
    color: match[1],
    pixels: Number(match[2]),
    bounds: {
      minX: Number(match[3]),
      minY: Number(match[4]),
      maxX: Number(match[5]),
      maxY: Number(match[6]),
    },
    framebuffer: {
      width: Number(match[7]),
      height: Number(match[8]),
    },
  };
}

async function requestPlotText(consoleMessages, previousCount) {
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).at(-1) || '';
  const match = message.match(
    /legend_layers=(\d+) marker_labels=(\d+) alpha_pixels=(\d+) vertices=(\d+) atlas=(\d+)x(\d+) rect=([^,]+),([^,]+),([^x]+)x([^ ]+) raster_scale=([^,]+),([^ ]+)/,
  );
  expect(match, `unparseable plot-text message: ${message}`).not.toBeNull();
  return {
    legendLayers: Number(match[1]),
    markerLabels: Number(match[2]),
    alphaPixels: Number(match[3]),
    vertices: Number(match[4]),
    atlas: { width: Number(match[5]), height: Number(match[6]) },
    rect: {
      x: Number(match[7]),
      y: Number(match[8]),
      width: Number(match[9]),
      height: Number(match[10]),
    },
    rasterScale: { x: Number(match[11]), y: Number(match[12]) },
  };
}

async function requestPlotTextPixels(consoleMessages, previousCount) {
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).at(-1) || '';
  const match = message.match(
    /pixels=(\d+) bounds=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) framebuffer=(\d+)x(\d+)/,
  );
  expect(match, `unparseable plot-text readback: ${message}`).not.toBeNull();
  return {
    pixels: Number(match[1]),
    bounds: {
      minX: Number(match[2]),
      minY: Number(match[3]),
      maxX: Number(match[4]),
      maxY: Number(match[5]),
    },
    framebuffer: { width: Number(match[6]), height: Number(match[7]) },
  };
}

async function requestPlotHover(page, consoleMessages, previousStateCount, previousPixelCount) {
  await page.evaluate(() => window.pjWasmReportPlotStateProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER enabled=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousStateCount);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousPixelCount);
  const stateMessage =
    consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER enabled=')).at(-1) || '';
  const stateMatch = stateMessage.match(
    /enabled=(\d+) visible=(\d+) point=([^,]+),([^ ]+) label=([^\s]*)/,
  );
  expect(stateMatch, `unparseable plot-hover state: ${stateMessage}`).not.toBeNull();
  const pixelMessage = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER_PIXELS')).at(-1) || '';
  const pixelMatch = pixelMessage.match(
    /yellow_pixels=(\d+) yellow_bounds=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) border_pixels=(\d+) framebuffer=(\d+)x(\d+)/,
  );
  expect(pixelMatch, `unparseable plot-hover pixels: ${pixelMessage}`).not.toBeNull();
  return {
    enabled: stateMatch[1] === '1',
    visible: stateMatch[2] === '1',
    point: { x: Number(stateMatch[3]), y: Number(stateMatch[4]) },
    label: decodeURIComponent(stateMatch[5]),
    pixels: {
      yellow: Number(pixelMatch[1]),
      bounds: {
        minX: Number(pixelMatch[2]),
        minY: Number(pixelMatch[3]),
        maxX: Number(pixelMatch[4]),
        maxY: Number(pixelMatch[5]),
      },
      border: Number(pixelMatch[6]),
      framebuffer: { width: Number(pixelMatch[7]), height: Number(pixelMatch[8]) },
    },
  };
}

test('QRhi WebGL canvas renders an imported scalar curve', async ({ page }) => {
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
    name: 'plot.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,value,temp\n0,1,10\n1,2,20\n2,3,30\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('plot/temp:3@0..2000000000=10..30');

  // Create the first real PlotWidget through the user-facing placeholder. F9
  // is compiled only into the acceptance probe build and calls addCurve() on
  // that real plot, avoiding a synthetic Qt drag/drop gesture.
  { const icon = await placeholderIconCenter(page, screen, consoleMessages, 'plot');
    await page.mouse.click(icon.x, icon.y); }
  await page.waitForTimeout(1500);
  await page.keyboard.press('F9');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_RHI_READY')) || '',
    { timeout: 15000 },
  ).toContain('backend=OpenGLES2/WebGL');
  const readyMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_RHI_READY')) || '';
  expect(readyMessage).toContain('WebGL 2.0');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '',
    { timeout: 15000 },
  ).toContain('first_curve=plot/temp');
  const frameMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '';
  expect(frameMessage).toContain('curves=1');
  // The default 0..1 s view serves both on-screen samples plus the off-screen
  // right guard at t=2, so the whole three-row fixture reaches the canvas.
  expect(frameMessage).toContain('samples=3');
  expect(frameMessage).toContain('endpoints=0,10..2,30');
  const curveVertices = frameMessage.match(/curve_vertices=(\d+)/);
  expect(curveVertices).not.toBeNull();
  expect(Number(curveVertices[1])).toBeGreaterThan(0);
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_READBACK')) || '',
    { timeout: 15000 },
  ).toContain('curve_pixels=');
  const readbackMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_READBACK')) || '';
  const curvePixels = readbackMessage.match(/curve_pixels=(\d+)/);
  expect(curvePixels).not.toBeNull();
  expect(Number(curvePixels[1])).toBeGreaterThan(0);

  // PlotRhiCanvas schedules one full top-level repaint after its first
  // submitted frame, settling the raster->RHI compositor switch and
  // initializing untouched raster backing-store regions. Exercise a later
  // raster-only partial update and ensure it does not clear untouched UI
  // or the accelerated plot to black.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.waitForTimeout(500);
  const updatedWindow = await page.screenshot();
  // This full-window golden guards catastrophic raster/RHI compositor loss
  // (for example a black or stale plot surface), not pixel geometry. Exact
  // curve vertices and framebuffer pixels are asserted above, while normal Qt
  // layout rounding can move widget edges by a couple of pixels.
  expect(updatedWindow).toMatchSnapshot('wasm-rhi-window-after-partial-update.png', {
    maxDiffPixelRatio: 0.05,
  });
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_PROBE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('QRhi scalar reduction preserves an isolated extremum within its frame budget', async ({ page }) => {
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
    name: 'extrema.csv',
    mimeType: 'text/csv',
    buffer: makeExtremaCsv(),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('plugin=CSV Loader');

  { const icon = await placeholderIconCenter(page, screen, consoleMessages, 'plot');
    await page.mouse.click(icon.x, icon.y); }
  await page.waitForTimeout(1500);
  // F10 is the probe-build variant that also fits the complete series, so the
  // reducer receives all 6,001 points instead of the shared timeline's initial
  // two guard samples. Production builds expose neither shortcut.
  await page.keyboard.press('F10');

  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_FRAME_OK')).at(-1) || '',
    { timeout: 15000 },
  ).toMatch(/first_curve=extrema\/temp.*samples=[5-9]\d{3}/);
  const frameMessage = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_FRAME_OK')).at(-1) || '';
  const inputSamples = frameMessage.match(/samples=(\d+)/);
  const reducedSamples = frameMessage.match(/reduced_samples=(\d+)/);
  expect(inputSamples).not.toBeNull();
  expect(reducedSamples).not.toBeNull();
  expect(Number(inputSamples[1])).toBeGreaterThan(5000);
  expect(Number(reducedSamples[1])).toBeGreaterThan(0);
  expect(Number(reducedSamples[1])).toBeLessThan(Number(inputSamples[1]));
  expect(frameMessage).toContain('runs=1');
  expect(frameMessage).toContain('dropped_runs=0');
  expect(frameMessage).toContain('budget_limited_curves=0');
  expect(frameMessage).toContain('truncated=0');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_READBACK')) || '',
    { timeout: 15000 },
  ).toContain('curve_bounds=');
  const readbackMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_READBACK')) || '';
  const bounds = readbackMessage.match(/curve_bounds=(\d+),(\d+)\.\.(\d+),(\d+)/);
  expect(bounds).not.toBeNull();
  expect(Number(bounds[4]) - Number(bounds[2])).toBeGreaterThan(250);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_PROBE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('real curve drag and drop drives the QRhi plot lifecycle', async ({ page }) => {
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
    name: 'drag.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp,value\n0,10,1\n1,20,3\n2,30,2\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=drag/temp,drag/time,drag/value');

  // Drag the real scalar leaves from probe-reported row centers. No F9/F10 add
  // probe is used: these gestures must traverse CurveTreeView's production
  // QDrag MIME path and the placeholder/live-plot drop handlers.
  const placeholder = { x: screen.x + 820, y: screen.y + 390 };
  await dragQtCanvas(page, await curveRowCenter(page, screen, consoleMessages, 'drag.csv/drag/temp'), placeholder);

  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);

  let stateMessages = 0;
  let state = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(state.count).toBe(1);
  expect(state.titles).toEqual(['drag/temp']);

  const canvasCenter = {
    x: screen.x + state.canvas.x + (state.canvas.width / 2),
    y: screen.y + state.canvas.y + (state.canvas.height / 2),
  };
  await dragQtCanvas(page, await curveRowCenter(page, screen, consoleMessages, 'drag.csv/drag/value'), canvasCenter);
  state = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(state.count).toBe(2);
  expect(state.titles).toEqual(['drag/temp', 'drag/value']);

  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
    { timeout: 10000 },
  ).toMatch(/drag\/temp:#[0-9a-f]{8}:[1-9]\d*,drag\/value:#[0-9a-f]{8}:[1-9]\d*/i);

  // Use the real canvas context menu. Qt WASM does not route End through a
  // non-blocking QMenu popup reliably, so click its final visible action.
  // This still exercises the production remove-all QAction; no probe mutates
  // plot state.
  await page.mouse.click(canvasCenter.x, canvasCenter.y, { button: 'right' });
  await page.waitForTimeout(250);
  const menuMessages = consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=')).length;
  await page.evaluate(() => window.pjWasmReportRemoveAllActionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(menuMessages);
  const menuMessage = consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=')).at(-1) || '';
  const actionCenter = menuMessage.match(/name=remove_all center=(\d+),(\d+)/);
  expect(actionCenter).not.toBeNull();
  await page.mouse.click(screen.x + Number(actionCenter[1]), screen.y + Number(actionCenter[2]));
  state = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(state.count).toBe(0);

  await dragQtCanvas(page, await curveRowCenter(page, screen, consoleMessages, 'drag.csv/drag/temp'), canvasCenter);
  state = await requestPlotState(page, consoleMessages, stateMessages);
  expect(state.count).toBe(1);
  expect(state.titles).toEqual(['drag/temp']);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('multi-selected scalar leaves drag to one plot in a single gesture', async ({ page }) => {
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
    name: 'multi-drag.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp,value\n0,10,1\n1,20,3\n2,30,2\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=multi-drag/temp,multi-drag/time,multi-drag/value');

  const tempRow = await curveRowCenter(page, screen, consoleMessages, 'multi-drag.csv/multi-drag/temp');
  const valueRow = await curveRowCenter(page, screen, consoleMessages, 'multi-drag.csv/multi-drag/value');
  await page.mouse.click(tempRow.x, tempRow.y);
  await page.keyboard.down('Control');
  await page.mouse.click(valueRow.x, valueRow.y);
  await page.keyboard.up('Control');

  await dragQtCanvas(page, tempRow, { x: screen.x + 820, y: screen.y + 390 });
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);
  const state = await requestPlotState(page, consoleMessages, 0);
  expect(state.count).toBe(2);
  expect(state.titles).toEqual(['multi-drag/temp', 'multi-drag/value']);
  expect(state.xy).toBe(false);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
    { timeout: 10000 },
  ).toMatch(/multi-drag\/temp:#[0-9a-f]{8}:[1-9]\d*,multi-drag\/value:#[0-9a-f]{8}:[1-9]\d*/i);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('real curve-style and width controls render every app-exposed QRhi variant', async ({ page }) => {
  test.setTimeout(150000);
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
    name: 'styles.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,signal\n0,1\n1,4\n2,2\n3,5\n4,0\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=styles/signal,styles/time');

  await dragQtCanvas(
    page,
    await curveRowCenter(page, screen, consoleMessages, 'styles.csv/styles/signal'),
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);

  let controlMessages = 0;
  let controls = await requestCurveStyleControls(page, consoleMessages, controlMessages);
  controlMessages += 1;
  if (!controls.open) {
    await page.mouse.click(screen.x + controls.panel.x, screen.y + controls.panel.y);
    await page.waitForTimeout(250);
    controls = await requestCurveStyleControls(page, consoleMessages, controlMessages);
    controlMessages += 1;
  }
  expect(controls.open).toBe(true);

  let stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  let pixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STYLE_PIXELS')).length;
  const styleSignatures = new Set();
  for (let style = 0; style < controls.styles.length; ++style) {
    const button = controls.styles[style];
    await page.mouse.click(screen.x + button.x, screen.y + button.y);
    const state = await requestPlotState(page, consoleMessages, stateMessages);
    stateMessages += 1;
    const readback = await requestPlotStylePixels(consoleMessages, pixelMessages);
    pixelMessages += 1;
    expect(state.style).toBe(style);
    expect(state.width).toBe(0);
    expect(state.xy).toBe(false);
    expect(readback.style).toBe(style);
    expect(readback.width).toBe(0);
    expect(readback.pixels).toBeGreaterThan(0);
    styleSignatures.add(`${readback.pixels}:${readback.hash}`);
  }
  expect(styleSignatures.size).toBe(6);

  await page.mouse.click(screen.x + controls.styles[0].x, screen.y + controls.styles[0].y);
  const widthSignatures = new Set();
  const widthPixels = [];
  let thickLineState = null;
  for (let width = 0; width < controls.widths.length; ++width) {
    const button = controls.widths[width];
    await page.mouse.click(screen.x + button.x, screen.y + button.y);
    const state = await requestPlotState(page, consoleMessages, stateMessages);
    stateMessages += 1;
    const readback = await requestPlotStylePixels(consoleMessages, pixelMessages);
    pixelMessages += 1;
    expect(state.style).toBe(0);
    expect(state.width).toBe(width);
    expect(readback.style).toBe(0);
    expect(readback.width).toBe(width);
    expect(readback.pixels).toBeGreaterThan(0);
    thickLineState = state;
    widthPixels.push(readback.pixels);
    widthSignatures.add(`${readback.pixels}:${readback.hash}`);
  }
  expect(widthSignatures.size).toBe(4);
  expect(widthPixels.at(-1)).toBeGreaterThan(widthPixels[0]);

  const samplePixel = (state, x, y) => ({
    x: screen.x + state.canvas.x + (((x - state.view.left) / (state.view.right - state.view.left)) * state.canvas.width),
    y: screen.y + state.canvas.y + (((state.view.top - y) / (state.view.top - state.view.bottom)) * state.canvas.height),
  });
  const tightSampleImage = async (state, x, y) => {
    const point = samplePixel(state, x, y);
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
    return page.screenshot({
      clip: { x: Math.round(point.x) - 12, y: Math.round(point.y) - 12, width: 24, height: 24 },
    });
  };
  const bevelImage = await tightSampleImage(thickLineState, 1, 4);
  expect(bevelImage).toMatchSnapshot('wasm-rhi-thick-line-bevel.png', { maxDiffPixels: 4 });

  await page.mouse.click(screen.x + controls.styles[1].x, screen.y + controls.styles[1].y);
  const dotState = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const dotReadback = await requestPlotStylePixels(consoleMessages, pixelMessages);
  pixelMessages += 1;
  expect(dotState.style).toBe(1);
  expect(dotState.width).toBe(3);
  expect(dotReadback.pixels).toBeGreaterThan(0);
  const dotImage = await tightSampleImage(dotState, 1, 4);
  expect(dotImage).toMatchSnapshot('wasm-rhi-square-dot.png', { maxDiffPixels: 4 });

  const gridMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_GRID_PIXELS')).length;
  await page.mouse.click(screen.x + controls.grid.x, screen.y + controls.grid.y);
  await requestPlotState(page, consoleMessages, stateMessages);
  const grid = await requestPlotGridPixels(consoleMessages, gridMessages);
  expect(grid.major.pixels).toBeGreaterThan(0);
  expect(grid.major.column).toBeGreaterThanOrEqual(0);
  expect(grid.major.period).toBe(6);
  expect(grid.major.phaseMask).toBe(0b001111);
  expect(grid.major.onPhases).toBe(4);
  expect(grid.major.periodComparisons).toBeGreaterThan(100);
  expect(grid.major.periodMatches / grid.major.periodComparisons).toBeGreaterThanOrEqual(0.85);
  expect(grid.minor.pixels).toBeGreaterThan(0);
  expect(grid.minor.period).toBe(3);
  expect(grid.minor.phaseMask).toBe(0b001);
  expect(grid.minor.onPhases).toBe(1);
  expect(grid.minor.periodComparisons).toBeGreaterThan(100);
  expect(grid.minor.periodMatches / grid.minor.periodComparisons).toBeGreaterThanOrEqual(0.85);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_CURVE_STYLE_CONTROLS_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('dense dotted curves reduce transactionally without committing a front-loaded prefix', async ({ page }) => {
  test.setTimeout(150000);
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
    name: 'dense-dash.csv',
    mimeType: 'text/csv',
    buffer: makeDenseDashCsv(),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 30000 },
  ).toContain('dense-dash/temp:20001@');

  // The reduction probe creates the real plot/curve and fits the complete
  // series. Only the pen mutation is probe-only; rendering and budgeting use
  // the same Qwt item and PlotRhiCanvas path as production.
  { const icon = await placeholderIconCenter(page, screen, consoleMessages, 'plot');
    await page.mouse.click(icon.x, icon.y); }
  await page.waitForTimeout(1500);
  await page.keyboard.press('F10');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_FRAME_OK')).at(-1) || '',
    { timeout: 30000 },
  ).toContain('first_curve=dense-dash/temp');

  const frameMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_FRAME_OK')).length;
  await page.evaluate(() => window.pjWasmSetFirstCurveDottedProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_CURVE_DOTTED')).at(-1) || '',
    { timeout: 10000 },
  ).toContain('title=dense-dash/temp');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_FRAME_OK')).length,
    { timeout: 30000 },
  ).toBeGreaterThan(frameMessages);
  const frame = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_FRAME_OK')).at(-1) || '';
  const telemetry = frame.match(
    /budget_limited_curves=(\d+) budget_reduced_curves=(\d+) budget_reduction_retries=(\d+) budget_discarded_trial_vertices=(\d+) budget_dropped_samples=(\d+) budget_omitted_curves=(\d+)/,
  );
  expect(telemetry, `unparseable budget telemetry: ${frame}`).not.toBeNull();
  expect(Number(telemetry[1])).toBe(0);
  expect(Number(telemetry[2])).toBe(1);
  expect(Number(telemetry[3])).toBeGreaterThan(0);
  expect(Number(telemetry[4])).toBeGreaterThan(0);
  expect(Number(telemetry[5])).toBeGreaterThan(0);
  expect(Number(telemetry[6])).toBe(0);
  expect(frame).toContain('truncated=0');

  const styleMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STYLE_PIXELS')).length;
  await page.evaluate(() => window.pjWasmReportPlotStateProbe());
  const readback = await requestPlotStylePixels(consoleMessages, styleMessages);
  expect(readback.pixels).toBeGreaterThan(0);
  expect(readback.bounds.minX).toBeLessThan(0.05 * readback.framebuffer.width);
  expect(readback.bounds.maxX).toBeGreaterThan(0.95 * readback.framebuffer.width);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_CURVE_DOTTED_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('huge offscreen dash phases remain bounded on a visible local segment', async ({ page }) => {
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
    name: 'huge-dash-phase.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp\n0,1e100\n1,0\n2,0.5\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('huge-dash-phase/temp:3@');

  { const icon = await placeholderIconCenter(page, screen, consoleMessages, 'plot');
    await page.mouse.click(icon.x, icon.y); }
  await page.waitForTimeout(1500);
  await page.keyboard.press('F10');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_RHI_READY')).at(-1) || '',
    { timeout: 15000 },
  ).toContain('backend=OpenGLES2/WebGL');

  await page.evaluate(() => window.pjWasmSetExtremeOffscreenViewProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_EXTREME_OFFSCREEN_VIEW')).at(-1) || '',
    { timeout: 10000 },
  ).toContain('title=huge-dash-phase/temp');
  const stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  const styleMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STYLE_PIXELS')).length;
  const state = await requestPlotState(page, consoleMessages, stateMessages);
  const readback = await requestPlotStylePixels(consoleMessages, styleMessages);
  expect(Math.abs(state.view.left)).toBeLessThan(1e-12);
  expect(Math.abs(state.view.right - 2)).toBeLessThan(1e-12);
  expect(Math.abs(state.view.bottom + 1)).toBeLessThan(1e-12);
  expect(Math.abs(state.view.top - 1)).toBeLessThan(1e-12);
  expect(readback.pixels).toBeGreaterThan(0);
  expect(readback.bounds.minX).toBeGreaterThan(0.40 * readback.framebuffer.width);
  expect(readback.bounds.maxX).toBeGreaterThan(0.90 * readback.framebuffer.width);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_EXTREME_OFFSCREEN_VIEW_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('right-drag creates XY curves through nonblocking dialogs and rolls back cancel', async ({ page }) => {
  test.setTimeout(150000);
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
    name: 'xy.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,ax,ay\n0,0,0\n1,1,1\n2,2,4\n3,3,9\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=xy/ax,xy/ay,xy/time');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  const axRow = { x: screen.x + 95, y: screen.y + 232 };
  const ayRow = { x: screen.x + 95, y: screen.y + 252 };
  const placeholder = { x: screen.x + 820, y: screen.y + 390 };
  await page.mouse.click(axRow.x, axRow.y);
  await page.keyboard.down('Control');
  await page.mouse.click(ayRow.x, ayRow.y);
  await page.keyboard.up('Control');

  // First exercise cancellation on the placeholder route. The temporary empty
  // plot must disappear again, leaving the dock reusable for the next drop.
  await dragQtCanvas(page, axRow, placeholder, 'right');
  let dialogMessages = 0;
  let dialog = await requestXyDialog(page, consoleMessages, dialogMessages);
  dialogMessages += 1;
  expect(dialog.x).toMatch(/ax$/);
  expect(dialog.y).toMatch(/ay$/);
  expect(dialog.alias).toBe('xy/a[x;y]');
  await page.keyboard.press('Escape');
  await page.waitForTimeout(250);
  let multi = await requestMultiPlotState(page, consoleMessages, 0);
  expect(multi.plots).toHaveLength(0);
  expect(multi.docks).toHaveLength(1);
  expect(multi.docks[0].kind).toBe('placeholder');

  // Repeat the same real right-drag and accept the production XY dialog.
  await dragQtCanvas(page, axRow, placeholder, 'right');
  dialog = await requestXyDialog(page, consoleMessages, dialogMessages);
  dialogMessages += 1;
  const firstAlias = dialog.alias;
  await page.mouse.click(screen.x + dialog.ok.x, screen.y + dialog.ok.y);

  let stateMessages = 0;
  let state = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(state.count).toBe(1);
  expect(state.titles).toEqual([firstAlias]);
  expect(state.xy).toBe(true);

  // A repeated pair starts with the same alias. Accepting it must open the
  // asynchronous duplicate warning, then restore the SAME XY dialog. Swap the
  // axes, edit the retained dialog's alias, and accept a second curve.
  const canvasCenter = {
    x: screen.x + state.canvas.x + (state.canvas.width / 2),
    y: screen.y + state.canvas.y + (state.canvas.height / 2),
  };
  await dragQtCanvas(page, axRow, canvasCenter, 'right');
  dialog = await requestXyDialog(page, consoleMessages, dialogMessages);
  dialogMessages += 1;
  expect(dialog.alias).toBe(firstAlias);
  await page.mouse.click(screen.x + dialog.ok.x, screen.y + dialog.ok.y);

  const warning = await requestXyDuplicateWarning(page, consoleMessages, 0);
  expect(warning.title).toBe('Duplicate name');
  expect(warning.body).toContain(firstAlias);
  await page.mouse.click(screen.x + warning.ok.x, screen.y + warning.ok.y);
  await page.waitForTimeout(200);

  dialog = await requestXyDialog(page, consoleMessages, dialogMessages);
  dialogMessages += 1;
  expect(dialog.alias).toBe(firstAlias);
  await page.mouse.click(screen.x + dialog.swap.x, screen.y + dialog.swap.y);
  await page.mouse.click(screen.x + dialog.aliasEdit.x, screen.y + dialog.aliasEdit.y);
  await page.keyboard.press('Control+A');
  await page.keyboard.type('xy-swapped');
  await page.mouse.click(screen.x + dialog.ok.x, screen.y + dialog.ok.y);

  state = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(state.count).toBe(2);
  expect(state.titles).toEqual([firstAlias, 'xy-swapped']);
  expect(state.xy).toBe(true);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
    { timeout: 10000 },
  ).toMatch(/:[1-9]\d*,xy-swapped:#[0-9a-f]{8}:[1-9]\d*/i);

  // Cancelling on an already-live XY plot keeps its mode and both curves.
  await dragQtCanvas(page, axRow, canvasCenter, 'right');
  dialog = await requestXyDialog(page, consoleMessages, dialogMessages);
  dialogMessages += 1;
  await page.keyboard.press('Escape');
  await page.waitForTimeout(150);
  state = await requestPlotState(page, consoleMessages, stateMessages);
  expect(state.count).toBe(2);
  expect(state.titles).toEqual([firstAlias, 'xy-swapped']);
  expect(state.xy).toBe(true);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_XY_DIALOG_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_XY_WARNING_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('Escape-cancelled curve drag adds nothing and leaves the tree grab balanced', async ({ page }) => {
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
    name: 'cancel-drag.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp,value\n0,10,1\n1,20,3\n2,30,2\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=cancel-drag/temp,cancel-drag/time,cancel-drag/value');

  // Expand dataset -> topic, same as the successful-drag scenario.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);

  // Enter the real placeholder drop target, then cancel while the initiating
  // button is still held. This must exercise CurveTreeView::event's Escape
  // path: abortWasmDrag sends DragLeave, releases the MIME payload, and
  // balances QAbstractItemView's source press without ever sending Drop.
  const source = { x: screen.x + 95, y: screen.y + 232 };
  const placeholder = { x: screen.x + 820, y: screen.y + 390 };
  await page.mouse.move(source.x, source.y);
  await page.mouse.down({ button: 'left' });
  await page.waitForTimeout(150);
  await page.mouse.move(source.x + 80, source.y + 20, { steps: 6 });
  await page.mouse.move(placeholder.x, placeholder.y, { steps: 18 });
  await page.waitForTimeout(250);
  await page.keyboard.press('Escape');
  await page.mouse.up({ button: 'left' });

  // No plot ever gets created, so there is nothing for F8 to report against;
  // give the (absent) drop a moment to settle instead of racing a marker
  // that this scenario must NOT produce.
  await page.waitForTimeout(750);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);

  // Prove the grab was released cleanly: a subsequent plain drag from the
  // same source row must add exactly one curve, not a stale multi-row
  // selection spanning the earlier (unbalanced) press point.
  await dragQtCanvas(page, source, placeholder);

  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);
  const state = await requestPlotState(page, consoleMessages, 0);
  expect(state.count).toBe(1);
  expect(state.titles).toEqual(['cancel-drag/temp']);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('real rectangle/wheel zoom, Ctrl-drag pan, and reset navigate the QRhi plot', async ({ page }) => {
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
    name: 'navigate.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp\n0,10\n1,20\n2,15\n3,30\n4,25\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=navigate/temp,navigate/time');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);
  let stateMessages = 0;
  const initial = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const canvasCenter = {
    x: screen.x + initial.canvas.x + (initial.canvas.width / 2),
    y: screen.y + initial.canvas.y + (initial.canvas.height / 2),
  };
  const initialWidth = initial.view.right - initial.view.left;
  const initialHeight = initial.view.top - initial.view.bottom;

  const rectangleStart = {
    x: screen.x + initial.canvas.x + (0.20 * initial.canvas.width),
    y: screen.y + initial.canvas.y + (0.20 * initial.canvas.height),
  };
  const rectangleEnd = {
    x: screen.x + initial.canvas.x + (0.75 * initial.canvas.width),
    y: screen.y + initial.canvas.y + (0.75 * initial.canvas.height),
  };
  await page.mouse.move(rectangleStart.x, rectangleStart.y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(rectangleEnd.x, rectangleEnd.y, { steps: 18 });
  // A newly dirtied QRhi surface can reach the DevTools capture path one
  // compositor frame after Qt's framebuffer probe. Prime that boundary so
  // the clipped snapshot records the live drag overlay instead of its clear.
  await page.screenshot();
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(
    () => requestAnimationFrame(resolve),
  )));
  const rectangleCorner = await page.screenshot({
    clip: {
      x: Math.round(rectangleStart.x) - 24,
      y: Math.round(rectangleStart.y) - 24,
      width: 48,
      height: 48,
    },
  });
  expect(rectangleCorner).toMatchSnapshot('wasm-rhi-navigation-rectangle-corner.png', { maxDiffPixels: 24 });
  await page.mouse.up({ button: 'left' });
  const rectangleZoomed = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const rectangleWidth = rectangleZoomed.view.right - rectangleZoomed.view.left;
  const rectangleHeight = rectangleZoomed.view.top - rectangleZoomed.view.bottom;
  expect(rectangleWidth).toBeLessThan(initialWidth);
  expect(rectangleHeight).toBeLessThan(initialHeight);

  await page.mouse.move(canvasCenter.x, canvasCenter.y);
  await page.mouse.wheel(0, -600);
  const zoomed = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const zoomedWidth = zoomed.view.right - zoomed.view.left;
  const zoomedHeight = zoomed.view.top - zoomed.view.bottom;
  expect(zoomedWidth).toBeLessThan(rectangleWidth);
  expect(zoomedHeight).toBeLessThan(rectangleHeight);

  await page.keyboard.down('Control');
  await page.mouse.move(canvasCenter.x, canvasCenter.y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(canvasCenter.x + 120, canvasCenter.y, { steps: 12 });
  await page.mouse.up({ button: 'left' });
  await page.keyboard.up('Control');
  const panned = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const pannedWidth = panned.view.right - panned.view.left;
  expect(Math.abs(pannedWidth - zoomedWidth)).toBeLessThan(1e-9);
  expect(Math.abs(panned.view.left - zoomed.view.left)).toBeGreaterThan(0.01 * zoomedWidth);

  await page.mouse.click(canvasCenter.x, canvasCenter.y, { button: 'right' });
  await page.waitForTimeout(250);
  const menuMessages = consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=')).length;
  await page.evaluate(() => window.pjWasmReportZoomOutActionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(menuMessages);
  const menuMessage = consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=')).at(-1) || '';
  const actionCenter = menuMessage.match(/name=zoom_out center=(\d+),(\d+)/);
  expect(actionCenter).not.toBeNull();
  await page.mouse.click(screen.x + Number(actionCenter[1]), screen.y + Number(actionCenter[2]));

  const reset = await requestPlotState(page, consoleMessages, stateMessages);
  expect(Math.abs(reset.view.left - initial.view.left)).toBeLessThan(1e-9);
  expect(Math.abs(reset.view.right - initial.view.right)).toBeLessThan(1e-9);
  expect(Math.abs(reset.view.bottom - initial.view.bottom)).toBeLessThan(1e-9);
  expect(Math.abs(reset.view.top - initial.view.top)).toBeLessThan(1e-9);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('Shift-drag seeks playback and moves the QRhi tracker without changing the view', async ({ page }) => {
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
    name: 'tracker.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp\n0,10\n1,20\n2,15\n3,30\n4,25\n5,35\n6,20\n7,40\n8,30\n9,45\n10,35\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=tracker/temp,tracker/time');

  // Build the plot through the same production tree drag/drop path as W8c.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);

  let stateMessages = 0;
  let trackerMessages = 0;
  let textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
  let textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
  await page.evaluate(() => window.pjWasmShowLegendProbe());
  const initial = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  await requestTrackerReadback(consoleMessages, trackerMessages);
  trackerMessages += 1;
  const initialText = await requestPlotText(consoleMessages, textMessages);
  textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
  const initialTextPixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);
  textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
  expect(initial.count).toBe(1);
  expect(initial.titles).toEqual(['tracker/temp']);
  expect(initial.trackerEnabled).toBe(true);
  expect(initial.trackerParameter).toBe(1);
  expect(initialText.legendLayers).toBe(1);
  // Before the first seek the tracker's logical label is outside the imported
  // range, so only the legend produces visible raster pixels.
  expect(initialText.markerLabels).toBe(0);
  expect(initialText.alphaPixels).toBeGreaterThan(0);
  expect(initialText.vertices).toBe(6);
  expect(initialText.atlas.width).toBeGreaterThan(0);
  expect(initialText.atlas.height).toBeGreaterThan(0);
  expect(initialText.rect.width).toBeGreaterThan(0);
  expect(initialText.rect.height).toBeGreaterThan(0);
  expect(Math.abs(initialText.rasterScale.x - 1)).toBeLessThan(0.05);
  expect(Math.abs(initialText.rasterScale.y - 1)).toBeLessThan(0.05);
  expect(initialTextPixels.pixels).toBeGreaterThan(0);

  const canvas = {
    x: screen.x + initial.canvas.x,
    y: screen.y + initial.canvas.y,
    width: initial.canvas.width,
    height: initial.canvas.height,
  };
  const viewWidth = initial.view.right - initial.view.left;
  const timeTolerance = (3 * viewWidth / initial.canvas.width) + 1e-9;
  const assertUnchangedView = (state) => {
    expect(Math.abs(state.view.left - initial.view.left)).toBeLessThan(1e-9);
    expect(Math.abs(state.view.right - initial.view.right)).toBeLessThan(1e-9);
    expect(Math.abs(state.view.bottom - initial.view.bottom)).toBeLessThan(1e-9);
    expect(Math.abs(state.view.top - initial.view.top)).toBeLessThan(1e-9);
  };
  const assertTrackerAt = (readback, ratio) => {
    // The UI framework's checked Highlight token replaced the retired legacy
    // purple constant; pin the semantic palette value used by the light theme.
    expect(readback.color.toLowerCase()).toBe('#ffff99ff');
    expect(readback.pixels).toBeGreaterThan(0);
    expect(readback.bounds.maxX - readback.bounds.minX).toBeLessThanOrEqual(
      Math.max(4, 0.01 * readback.framebuffer.width),
    );
    expect(readback.bounds.maxY - readback.bounds.minY).toBeGreaterThan(0.85 * readback.framebuffer.height);
    const trackerCenter = 0.5 * (readback.bounds.minX + readback.bounds.maxX);
    expect(Math.abs((trackerCenter / readback.framebuffer.width) - ratio)).toBeLessThan(0.025);
  };

  await seekQtPlot(page, canvas, 0.25, 0.70);
  const forward = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const forwardTracker = await requestTrackerReadback(consoleMessages, trackerMessages);
  trackerMessages += 1;
  const expectedForwardTime = initial.view.left + (0.70 * viewWidth);
  expect(Math.abs(forward.time - expectedForwardTime)).toBeLessThan(timeTolerance);
  assertUnchangedView(forward);
  assertTrackerAt(forwardTracker, 0.70);

  await seekQtPlot(page, canvas, 0.65, 0.40);
  const backward = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const backwardTracker = await requestTrackerReadback(consoleMessages, trackerMessages);
  const expectedBackwardTime = initial.view.left + (0.40 * viewWidth);
  expect(Math.abs(backward.time - expectedBackwardTime)).toBeLessThan(timeTolerance);
  expect(backward.time).toBeLessThan(forward.time);
  expect(backwardTracker.bounds.maxX).toBeLessThan(forwardTracker.bounds.minX);
  assertUnchangedView(backward);
  assertTrackerAt(backwardTracker, 0.40);

  // Cycle the real toolbar control through value+name, line-only, and back to
  // value. The QRhi text layer must follow Qwt marker visibility exactly while
  // the legend remains independently visible.
  textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
  // Forward/backward state probes also schedule framebuffer readbacks. Start
  // from the current count so this assertion cannot consume one of those older
  // legend-only samples when the browser runs the timers late.
  textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
  await page.evaluate(() => window.pjWasmCycleTimeTrackerProbe());
  const valueNameState = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const valueNameText = await requestPlotText(consoleMessages, textMessages);
  textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
  const valueNamePixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);
  textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
  expect(valueNameState.trackerParameter).toBe(2);
  expect(valueNameText.legendLayers).toBe(1);
  expect(valueNameText.markerLabels).toBe(1);
  expect(valueNameText.vertices).toBe(12);
  expect(valueNamePixels.pixels).toBeGreaterThan(0);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_DRAW_BATCHES')).at(-1) || '',
    { timeout: 10000 },
  ).toMatch(/sequence=GTG/);
  await page.screenshot();
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(
    () => requestAnimationFrame(resolve),
  )));
  const markerPoint = {
    x: canvas.x + (((backward.time - initial.view.left) / viewWidth) * canvas.width),
    y: canvas.y + (((initial.view.top - 25) / (initial.view.top - initial.view.bottom)) * canvas.height),
  };
  const textImage = await page.screenshot({
    clip: {
      x: Math.round(markerPoint.x) - 4,
      y: Math.round(markerPoint.y) - 22,
      width: 190,
      height: 44,
    },
  });
  expect(textImage).toMatchSnapshot('wasm-rhi-plot-text-marker.png', { maxDiffPixels: 200 });

  await page.evaluate(() => window.pjWasmCycleTimeTrackerProbe());
  const lineOnlyState = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const lineOnlyText = await requestPlotText(consoleMessages, textMessages);
  textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
  const lineOnlyPixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);
  textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
  expect(lineOnlyState.trackerParameter).toBe(0);
  expect(lineOnlyText.legendLayers).toBe(1);
  expect(lineOnlyText.markerLabels).toBe(0);
  expect(lineOnlyText.vertices).toBe(6);
  expect(lineOnlyText.alphaPixels).toBeLessThan(valueNameText.alphaPixels);
  expect(lineOnlyPixels.pixels).toBeLessThan(valueNamePixels.pixels);

  await page.evaluate(() => window.pjWasmCycleTimeTrackerProbe());
  const restoredState = await requestPlotState(page, consoleMessages, stateMessages);
  const restoredText = await requestPlotText(consoleMessages, textMessages);
  const restoredPixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);
  expect(restoredState.trackerParameter).toBe(1);
  expect(restoredText.legendLayers).toBe(1);
  expect(restoredText.markerLabels).toBe(1);
  expect(restoredText.vertices).toBe(12);
  expect(restoredText.alphaPixels).toBeGreaterThan(lineOnlyText.alphaPixels);
  expect(restoredPixels.pixels).toBeGreaterThan(lineOnlyPixels.pixels);

  // Fault-inject only the atlas stage: production geometry must stay visible,
  // stale text must not be drawn, the warning must latch, and clearing the
  // fault must rebuild from live Qwt state without a QRhi reinitialization.
  stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
  textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
  const curvePixelMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS'),
  ).length;
  const failureAttemptMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_TEXT_FAILURE_ATTEMPT'),
  ).length;
  await page.evaluate(() => window.pjWasmForceTextFailureProbe(true));
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_TEXT_FAILURE_MODE enabled=1')).at(-1) || '',
    { timeout: 10000 },
  ).toContain('canvases=1');
  await requestPlotState(page, consoleMessages, stateMessages);
  const degradedPixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(curvePixelMessages);
  expect(
    consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
  ).toMatch(/tracker\/temp:#[0-9a-f]{8}:[1-9]\d*/i);
  expect(degradedPixels.pixels).toBeLessThan(restoredPixels.pixels);
  expect(consoleMessages.filter(message => message.includes('rendering geometry without text')).length).toBe(1);
  expect(consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length).toBe(textMessages);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_TEXT_FAILURE_ATTEMPT')).at(-1) || '',
    { timeout: 10000 },
  ).toContain('count=2');
  const attemptsAfterRetry = consoleMessages.filter(
    message => message.includes('PJ_WASM_TEXT_FAILURE_ATTEMPT'),
  ).length;
  expect(attemptsAfterRetry - failureAttemptMessages).toBe(2);
  await page.waitForTimeout(250);
  expect(consoleMessages.filter(message => message.includes('PJ_WASM_TEXT_FAILURE_ATTEMPT')).length)
    .toBe(attemptsAfterRetry);

  stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
  await page.evaluate(() => window.pjWasmForceTextFailureProbe(false));
  await requestPlotState(page, consoleMessages, stateMessages);
  const recoveredText = await requestPlotText(consoleMessages, textMessages);
  const recoveredPixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);
  expect(recoveredText.legendLayers).toBe(1);
  expect(recoveredText.markerLabels).toBe(1);
  expect(recoveredText.vertices).toBe(12);
  expect(recoveredPixels.pixels).toBeGreaterThan(degradedPixels.pixels);

  // A missing QRhi pipeline is a whole-render failure rather than the
  // geometry-only text degradation above. Even on this early return the valid
  // render target must receive a defined clear, never retain the old curve.
  stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  let pipelineCurveMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS'),
  ).length;
  const pipelineFailures = consoleMessages.filter(
    message => message.includes('PJ_WASM_PLOT_RHI_FAILED'),
  ).length;
  await page.evaluate(() => window.pjWasmForcePipelineFailureProbe(true));
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PIPELINE_FAILURE_MODE enabled=1')).at(-1) || '',
    { timeout: 10000 },
  ).toContain('canvases=1');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_RHI_FAILED')).length,
    { timeout: 10000 },
  ).toBe(pipelineFailures + 1);
  await requestPlotState(page, consoleMessages, stateMessages);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(pipelineCurveMessages);
  expect(
    consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
  ).toMatch(/tracker\/temp:#[0-9a-f]{8}:0/i);

  stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  pipelineCurveMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS'),
  ).length;
  await page.evaluate(() => window.pjWasmForcePipelineFailureProbe(false));
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PIPELINE_FAILURE_MODE enabled=0')).at(-1) || '',
    { timeout: 10000 },
  ).toContain('canvases=1');
  await requestPlotState(page, consoleMessages, stateMessages);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(pipelineCurveMessages);
  expect(
    consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
  ).toMatch(/tracker\/temp:#[0-9a-f]{8}:[1-9]\d*/i);
  expect(consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_RHI_FAILED')).length)
    .toBe(pipelineFailures + 1);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_TRACKER_CYCLE_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_LEGEND_SHOW_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test.describe('HiDPI QRhi text', () => {
  test.use({ deviceScaleFactor: 2 });

  test('rasterizes legend and tracker text at the render target pixel ratio', async ({ page }) => {
    test.setTimeout(120000);
    const errors = [];
    const consoleMessages = [];
    await page.addInitScript(() => { delete window.showOpenFilePicker; });
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => consoleMessages.push(message.text()));

    await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
    await waitForQtApp(page, consoleMessages);
    expect(await page.evaluate(() => window.devicePixelRatio)).toBe(2);
    const screen = await page.locator('#screen').boundingBox();
    expect(screen).not.toBeNull();

    const chooser = await openFileChooser(page, screen);
    await chooser.setFiles({
      name: 'hidpi-text.csv',
      mimeType: 'text/csv',
      buffer: Buffer.from('time,temp\n0,10\n1,20\n2,15\n3,30\n4,25\n5,35\n'),
    });
    await page.waitForTimeout(1000);
    await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
    await expect.poll(
      () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
      { timeout: 15000 },
    ).toContain('scalar_series=hidpi-text/temp,hidpi-text/time');

    await page.mouse.click(screen.x + 10, screen.y + 192);
    await page.mouse.click(screen.x + 30, screen.y + 212);
    await dragQtCanvas(
      page,
      { x: screen.x + 95, y: screen.y + 232 },
      { x: screen.x + 820, y: screen.y + 390 },
    );
    await expect.poll(
      () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
      { timeout: 15000 },
    ).toBe(true);

    let stateMessages = 0;
    let textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
    let textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
    await page.evaluate(() => window.pjWasmShowLegendProbe());
    const state = await requestPlotState(page, consoleMessages, stateMessages);
    stateMessages += 1;
    await requestPlotText(consoleMessages, textMessages);
    textMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_OK')).length;
    const initialPixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);
    textPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_TEXT_PIXELS')).length;
    const canvas = {
      x: screen.x + state.canvas.x,
      y: screen.y + state.canvas.y,
      width: state.canvas.width,
      height: state.canvas.height,
    };
    await seekQtPlot(page, canvas, 0.25, 0.55);
    await requestPlotState(page, consoleMessages, stateMessages);
    const text = await requestPlotText(consoleMessages, textMessages);
    const pixels = await requestPlotTextPixels(consoleMessages, textPixelMessages);

    expect(text.legendLayers).toBe(1);
    expect(text.markerLabels).toBe(1);
    expect(text.vertices).toBe(12);
    expect(pixels.pixels).toBeGreaterThan(initialPixels.pixels);
    expect(Math.abs((pixels.framebuffer.width / state.canvas.width) - 2)).toBeLessThan(0.05);
    expect(Math.abs((pixels.framebuffer.height / state.canvas.height) - 2)).toBeLessThan(0.05);
    // This is measured from the physical cropped layer images versus their
    // logical QRhi quads, so a 1x atlas merely upscaled by the shader fails.
    expect(Math.abs(text.rasterScale.x - 2)).toBeLessThan(0.05);
    expect(Math.abs(text.rasterScale.y - 2)).toBeLessThan(0.05);
    expect(text.atlas.width * text.atlas.height).toBeLessThan(
      0.25 * pixels.framebuffer.width * pixels.framebuffer.height,
    );
    expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
    expect(consoleMessages.some(message => message.includes('PJ_WASM_LEGEND_SHOW_FAILED'))).toBe(false);
    expect(errors).toEqual([]);
  });
});

test.describe('sub-1 DPR QRhi cosmetics', () => {
  test.use({ deviceScaleFactor: 0.75 });

  test('keeps zero-width grid pens one physical pixel with standard dash cadence', async ({ page }) => {
    test.setTimeout(120000);
    const errors = [];
    const consoleMessages = [];
    await page.addInitScript(() => { delete window.showOpenFilePicker; });
    page.on('pageerror', error => errors.push(String(error)));
    page.on('console', message => consoleMessages.push(message.text()));

    await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
    await waitForQtApp(page, consoleMessages);
    expect(await page.evaluate(() => window.devicePixelRatio)).toBe(0.75);
    const screen = await page.locator('#screen').boundingBox();
    expect(screen).not.toBeNull();

    const chooser = await openFileChooser(page, screen);
    await chooser.setFiles({
      name: 'lodpi-grid.csv',
      mimeType: 'text/csv',
      buffer: Buffer.from('time,temp\n0,1\n1,4\n2,2\n3,5\n4,0\n'),
    });
    await page.waitForTimeout(1000);
    await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
    await expect.poll(
      () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
      { timeout: 15000 },
    ).toContain('scalar_series=lodpi-grid/temp,lodpi-grid/time');

    { const icon = await placeholderIconCenter(page, screen, consoleMessages, 'plot');
    await page.mouse.click(icon.x, icon.y); }
    await page.waitForTimeout(1500);
    await page.keyboard.press('F10');
    await expect.poll(
      () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_FRAME_OK')).at(-1) || '',
      { timeout: 15000 },
    ).toContain('first_curve=lodpi-grid/temp');

    let controlMessages = 0;
    let controls = await requestCurveStyleControls(page, consoleMessages, controlMessages);
    controlMessages += 1;
    if (!controls.open) {
      await page.mouse.click(screen.x + controls.panel.x, screen.y + controls.panel.y);
      await page.waitForTimeout(250);
      controls = await requestCurveStyleControls(page, consoleMessages, controlMessages);
    }
    expect(controls.open).toBe(true);

    const stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
    const styleMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STYLE_PIXELS')).length;
    const gridMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_GRID_PIXELS')).length;
    await page.mouse.click(screen.x + controls.grid.x, screen.y + controls.grid.y);
    const state = await requestPlotState(page, consoleMessages, stateMessages);
    const readback = await requestPlotStylePixels(consoleMessages, styleMessages);
    const grid = await requestPlotGridPixels(consoleMessages, gridMessages);

    expect(Math.abs((readback.framebuffer.width / state.canvas.width) - 0.75)).toBeLessThan(0.02);
    expect(Math.abs((readback.framebuffer.height / state.canvas.height) - 0.75)).toBeLessThan(0.02);
    expect(grid.major.pixels).toBeGreaterThan(0);
    expect(grid.major.period).toBe(6);
    expect(grid.major.phaseMask).toBe(0b001111);
    expect(grid.major.onPhases).toBe(4);
    expect(grid.major.periodComparisons).toBeGreaterThan(75);
    expect(grid.major.periodMatches / grid.major.periodComparisons).toBeGreaterThanOrEqual(0.80);
    expect(grid.minor.pixels).toBeGreaterThan(0);
    expect(grid.minor.period).toBe(3);
    expect(grid.minor.phaseMask).toBe(0b001);
    expect(grid.minor.onPhases).toBe(1);
    expect(grid.minor.periodComparisons).toBeGreaterThan(75);
    expect(grid.minor.periodMatches / grid.minor.periodComparisons).toBeGreaterThanOrEqual(0.80);

    expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
    expect(errors).toEqual([]);
  });
});

test('mouse hover selects a curve point and renders its QRhi inspector', async ({ page }) => {
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
    name: 'hover.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp\n0,10\n1,20\n2,15\n3,30\n4,25\n5,35\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=hover/temp,hover/time');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);

  const state = await requestPlotState(page, consoleMessages, 0);
  await page.waitForTimeout(350);
  const canvas = {
    x: screen.x + state.canvas.x,
    y: screen.y + state.canvas.y,
    width: state.canvas.width,
    height: state.canvas.height,
  };
  const sample = { x: 3, y: 30 };
  const samplePixel = {
    x: canvas.x + (((sample.x - state.view.left) / (state.view.right - state.view.left)) * canvas.width),
    y: canvas.y + (((state.view.top - sample.y) / (state.view.top - state.view.bottom)) * canvas.height),
  };
  let hoverMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER enabled=')).length;
  let hoverPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER_PIXELS')).length;

  await page.mouse.move(samplePixel.x, samplePixel.y);
  await page.waitForTimeout(150);
  let hovered;
  await expect.poll(async () => {
    hovered = await requestPlotHover(page, consoleMessages, hoverMessages, hoverPixelMessages);
    hoverMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER enabled=')).length;
    hoverPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER_PIXELS')).length;
    return hovered.pixels.yellow > 0 && hovered.pixels.border > 0;
  }, { timeout: 10000 }).toBe(true);
  const xTolerance = (2 * (state.view.right - state.view.left)) / canvas.width;
  const yTolerance = (2 * (state.view.top - state.view.bottom)) / canvas.height;
  expect(hovered.enabled).toBe(true);
  expect(hovered.visible).toBe(true);
  expect(Math.abs(hovered.point.x - sample.x)).toBeLessThan(xTolerance);
  expect(Math.abs(hovered.point.y - sample.y)).toBeLessThan(yTolerance);
  expect(hovered.label).toContain('hover/temp');
  expect(hovered.label).toContain('<br>x:');
  expect(hovered.label).toContain('<br>y:');
  expect(hovered.pixels.yellow).toBeGreaterThan(0);
  expect(hovered.pixels.border).toBeGreaterThan(0);
  expect(hovered.pixels.bounds.maxX).toBeGreaterThan(hovered.pixels.bounds.minX);
  expect(hovered.pixels.bounds.maxY).toBeGreaterThan(hovered.pixels.bounds.minY);
  await page.screenshot();
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(
    () => requestAnimationFrame(resolve),
  )));
  const hoverImage = await page.screenshot({
    clip: {
      x: Math.round(samplePixel.x) - 8,
      y: Math.round(samplePixel.y) - 50,
      width: 110,
      height: 64,
    },
  });
  expect(hoverImage).toMatchSnapshot('wasm-rhi-point-inspector-detail.png', { maxDiffPixels: 200 });

  await page.mouse.move(screen.x + 20, screen.y + 120);
  await page.waitForTimeout(150);
  const left = await requestPlotHover(page, consoleMessages, hoverMessages, hoverPixelMessages);
  hoverMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER enabled=')).length;
  hoverPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER_PIXELS')).length;
  expect(left.enabled).toBe(true);
  expect(left.visible).toBe(false);
  expect(left.point).toEqual({ x: 0, y: 0 });
  expect(left.label).toBe('');
  expect(left.pixels.yellow).toBe(0);
  expect(left.pixels.border).toBe(0);

  await page.mouse.move(samplePixel.x, samplePixel.y);
  await page.waitForTimeout(150);
  await page.evaluate(() => window.pjWasmToggleShowPointProbe());
  const disabled = await requestPlotHover(page, consoleMessages, hoverMessages, hoverPixelMessages);
  hoverMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER enabled=')).length;
  hoverPixelMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER_PIXELS')).length;
  expect(disabled.enabled).toBe(false);
  expect(disabled.visible).toBe(false);
  expect(disabled.point).toEqual({ x: 0, y: 0 });
  expect(disabled.label).toBe('');
  expect(disabled.pixels.yellow).toBe(0);
  expect(disabled.pixels.border).toBe(0);

  await page.evaluate(() => window.pjWasmToggleShowPointProbe());
  await page.mouse.move(screen.x + 20, screen.y + 120);
  await page.mouse.move(samplePixel.x, samplePixel.y);
  await page.waitForTimeout(150);
  const restored = await requestPlotHover(page, consoleMessages, hoverMessages, hoverPixelMessages);
  expect(restored.enabled).toBe(true);
  expect(restored.visible).toBe(true);
  expect(restored.pixels.yellow).toBeGreaterThan(0);
  expect(restored.pixels.border).toBeGreaterThan(0);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_SHOW_POINT_TOGGLE_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('two split QRhi plots render distinct curves and synchronize playback trackers', async ({ page }) => {
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
    name: 'multi-plot.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp,value\n0,10,100\n1,20,80\n2,15,120\n3,30,60\n4,25,140\n5,35,40\n6,20,160\n7,40,20\n8,30,180\n9,45,10\n10,35,200\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=multi-plot/temp,multi-plot/time,multi-plot/value');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_RHI_READY')).length,
    { timeout: 15000 },
  ).toBeGreaterThanOrEqual(1);

  let stateMessages = 0;
  const initial = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const initialCenter = {
    x: screen.x + initial.canvas.x + (initial.canvas.width / 2),
    y: screen.y + initial.canvas.y + (initial.canvas.height / 2),
  };

  // Invoke the real PlotWidget split QAction. The probe only reports its
  // geometry because a canvas menu has no DOM node for Playwright to locate.
  await page.mouse.click(initialCenter.x, initialCenter.y, { button: 'right' });
  await page.waitForTimeout(250);
  const splitMenuMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=split_horizontal'),
  ).length;
  await page.evaluate(() => window.pjWasmReportSplitHorizontalActionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=split_horizontal')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(splitMenuMessages);
  const splitMenuMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=split_horizontal'),
  ).at(-1) || '';
  const splitCenter = splitMenuMessage.match(/center=(\d+),(\d+)/);
  expect(splitCenter).not.toBeNull();
  await page.mouse.click(screen.x + Number(splitCenter[1]), screen.y + Number(splitCenter[2]));

  let multiMessages = 0;
  let split;
  const splitDeadline = Date.now() + 10000;
  do {
    split = await requestMultiPlotState(page, consoleMessages, multiMessages);
    multiMessages += 1;
    const currentPlaceholder = split.docks.find(dock => dock.kind === 'placeholder');
    if (split.docks.length === 2 && split.plots.length === 1 && currentPlaceholder?.rect.width > 200 &&
        currentPlaceholder.rect.height > 300) {
      break;
    }
    await page.waitForTimeout(100);
  } while (Date.now() < splitDeadline);
  expect(split.docks).toHaveLength(2);
  expect(split.plots).toHaveLength(1);
  // Use the last keyed observation immediately before the drop so an ADS
  // reflow cannot leave automation targeting the placeholder's stale geometry.
  const placeholder = split.docks.find(dock => dock.kind === 'placeholder');
  expect(placeholder).toBeDefined();
  expect(placeholder.rect.width).toBeGreaterThan(200);
  expect(placeholder.rect.height).toBeGreaterThan(300);

  // Populate the new sibling through the production CurveTreeView MIME path.
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 282 },
    {
      x: screen.x + placeholder.rect.x + (placeholder.rect.width / 2),
      y: screen.y + placeholder.rect.y + (placeholder.rect.height / 2),
    },
  );
  // The ADS split and the new QRhiWidget initialize asynchronously. Generic
  // READY logs cannot identify which canvas emitted them, so poll the keyed
  // per-canvas observation until both widgets have final-sized WebGL content.
  let populated;
  const populatedDeadline = Date.now() + 10000;
  do {
    populated = await requestMultiPlotState(page, consoleMessages, multiMessages);
    multiMessages += 1;
    const settled = populated.plots.length === 2 && populated.plots.every(
      plot => plot.backend.includes('OpenGLES2/WebGL') && plot.vertices > 0 &&
        plot.canvas.width > 200 && plot.canvas.height > 300,
    );
    if (settled) {
      break;
    }
    await page.waitForTimeout(100);
  } while (Date.now() < populatedDeadline);
  expect(populated.docks).toHaveLength(2);
  expect(populated.docks.every(dock => dock.kind === 'plot')).toBe(true);
  expect(populated.plots).toHaveLength(2);
  const tempPlot = populated.plots.find(plot => plot.titles.includes('multi-plot/temp'));
  const valuePlot = populated.plots.find(plot => plot.titles.includes('multi-plot/value'));
  expect(tempPlot).toBeDefined();
  expect(valuePlot).toBeDefined();
  expect(tempPlot.plotId).not.toBe(valuePlot.plotId);
  expect(tempPlot.backend).toContain('OpenGLES2/WebGL');
  expect(valuePlot.backend).toContain('OpenGLES2/WebGL');
  expect(tempPlot.vertices).toBeGreaterThan(0);
  expect(valuePlot.vertices).toBeGreaterThan(0);
  expect(tempPlot.canvas.width).toBeGreaterThan(200);
  expect(valuePlot.canvas.width).toBeGreaterThan(200);
  expect(tempPlot.canvas.height).toBeGreaterThan(300);
  expect(valuePlot.canvas.height).toBeGreaterThan(300);
  expect(tempPlot.readback.curves).toHaveLength(2);
  expect(valuePlot.readback.curves).toHaveLength(2);
  const tempOnTemp = tempPlot.readback.curves.find(curve => curve.title === 'multi-plot/temp');
  const valueOnTemp = tempPlot.readback.curves.find(curve => curve.title === 'multi-plot/value');
  const tempOnValue = valuePlot.readback.curves.find(curve => curve.title === 'multi-plot/temp');
  const valueOnValue = valuePlot.readback.curves.find(curve => curve.title === 'multi-plot/value');
  expect(tempOnTemp).toBeDefined();
  expect(valueOnTemp).toBeDefined();
  expect(tempOnValue).toBeDefined();
  expect(valueOnValue).toBeDefined();
  expect(tempOnTemp.color).not.toBe(valueOnValue.color);
  expect(tempOnTemp.pixels).toBeGreaterThan(0);
  expect(valueOnValue.pixels).toBeGreaterThan(0);
  expect(valueOnTemp.pixels).toBe(0);
  expect(tempOnValue.pixels).toBe(0);
  expect(tempPlot.canvas.x + tempPlot.canvas.width).toBeLessThanOrEqual(valuePlot.canvas.x);

  // Chromium's first DevTools capture after adding a second QRhiWidget can
  // sample mixed raster/RHI surfaces before its capture compositor synchronizes
  // them, even though the on-screen window and a subsequent capture are correct.
  // Prime that capture-only boundary, wait two browser frames, then snapshot the
  // stable full window. Keyed framebuffer checks above remain authoritative.
  await page.screenshot();
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(
    () => requestAnimationFrame(resolve),
  )));
  const twoPlotImage = await page.screenshot();
  // Exact per-canvas readbacks above verify the rendered data. This broader
  // golden catches a missing or stale composed surface while allowing minor
  // Qt layout rounding across otherwise identical browser runs.
  expect(twoPlotImage).toMatchSnapshot('wasm-rhi-two-plots.png', { maxDiffPixelRatio: 0.05 });

  // One global legend action must produce near-black legend pixels in each
  // keyed framebuffer. This observes the post-draw result per canvas, so two
  // unqualified CPU-atlas logs from one canvas cannot fake independence.
  expect(populated.plots.every(plot => plot.readback.textPixels === 0)).toBe(true);
  await page.evaluate(() => window.pjWasmShowLegendProbe());
  const withLegends = await requestMultiPlotState(page, consoleMessages, multiMessages);
  multiMessages += 1;
  expect(withLegends.plots).toHaveLength(2);
  for (const plot of withLegends.plots) {
    expect(plot.readback.textPixels).toBeGreaterThan(0);
    expect(plot.readback.textBounds.maxX).toBeGreaterThan(plot.readback.textBounds.minX);
    expect(plot.readback.textBounds.maxY).toBeGreaterThan(plot.readback.textBounds.minY);
  }

  const initialViews = new Map(populated.plots.map(plot => [plot.plotId, plot.view]));
  const absoluteCanvas = plot => ({
    x: screen.x + plot.canvas.x,
    y: screen.y + plot.canvas.y,
    width: plot.canvas.width,
    height: plot.canvas.height,
  });
  const assertSynchronizedTracker = (observation, playbackTime) => {
    for (const plot of observation.plots) {
      const previousView = initialViews.get(plot.plotId);
      expect(previousView).toBeDefined();
      expect(Math.abs(plot.view.left - previousView.left)).toBeLessThan(1e-9);
      expect(Math.abs(plot.view.right - previousView.right)).toBeLessThan(1e-9);
      expect(Math.abs(plot.view.bottom - previousView.bottom)).toBeLessThan(1e-9);
      expect(Math.abs(plot.view.top - previousView.top)).toBeLessThan(1e-9);
      expect(plot.readback.trackerPixels).toBeGreaterThan(0);
      expect(plot.readback.trackerBounds.maxX - plot.readback.trackerBounds.minX).toBeLessThanOrEqual(
        Math.max(4, 0.01 * plot.readback.framebuffer.width),
      );
      expect(plot.readback.trackerBounds.maxY - plot.readback.trackerBounds.minY).toBeGreaterThan(
        0.85 * plot.readback.framebuffer.height,
      );
      const expectedRatio = (playbackTime - plot.view.left) / (plot.view.right - plot.view.left);
      const trackerCenter = 0.5 * (plot.readback.trackerBounds.minX + plot.readback.trackerBounds.maxX);
      const trackerTolerance = Math.max(5 / plot.readback.framebuffer.width, 0.005);
      expect(Math.abs((trackerCenter / plot.readback.framebuffer.width) - expectedRatio)).toBeLessThan(
        trackerTolerance,
      );
      const ownCurve = plot.readback.curves.find(curve => plot.titles.includes(curve.title));
      expect(ownCurve).toBeDefined();
      expect(ownCurve.pixels).toBeGreaterThan(0);
    }
  };

  await seekQtPlot(page, absoluteCanvas(tempPlot), 0.25, 0.65);
  const forwardTime = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  const forward = await requestMultiPlotState(page, consoleMessages, multiMessages);
  multiMessages += 1;
  const expectedForward = tempPlot.view.left + (0.65 * (tempPlot.view.right - tempPlot.view.left));
  const forwardTolerance = (3 * (tempPlot.view.right - tempPlot.view.left) / tempPlot.canvas.width) + 1e-9;
  expect(Math.abs(forwardTime.time - expectedForward)).toBeLessThan(forwardTolerance);
  assertSynchronizedTracker(forward, forwardTime.time);

  const currentValuePlot = forward.plots.find(plot => plot.plotId === valuePlot.plotId);
  expect(currentValuePlot).toBeDefined();
  await seekQtPlot(page, absoluteCanvas(currentValuePlot), 0.60, 0.35);
  const backwardTime = await requestPlotState(page, consoleMessages, stateMessages);
  const backward = await requestMultiPlotState(page, consoleMessages, multiMessages);
  const expectedBackward = valuePlot.view.left + (0.35 * (valuePlot.view.right - valuePlot.view.left));
  const backwardTolerance = (3 * (valuePlot.view.right - valuePlot.view.left) / valuePlot.canvas.width) + 1e-9;
  expect(Math.abs(backwardTime.time - expectedBackward)).toBeLessThan(backwardTolerance);
  expect(backwardTime.time).toBeLessThan(forwardTime.time);
  assertSynchronizedTracker(backward, backwardTime.time);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_MULTI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('Failed to load bundled Noto Sans font'))).toBe(false);
  expect(errors).toEqual([]);
});

test('official MCAP scalar imports, renders through QRhi, and seeks playback', async ({ page }) => {
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
    name: 'ros2-float64.mcap',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('ros2_float64.mcap.b64'),
  });
  // The cancelled worker has released its progress/cancel windows, but Qt may
  // need one additional canvas frame before mapping the next large MCAP dialog.
  await page.waitForTimeout(2000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('/ros2/value/data:3@')) || '',
    { timeout: 30000 },
  ).toContain('=1.25..7.75');
  const imported = consoleMessages.find(message => message.includes('/ros2/value/data:3@')) || '';
  expect(imported).toContain('PJ_FILE_LOAD_OK plugin=MCAP Loader');
  expect(imported).toContain('scalar_series=/ros2/value/data');
  const sampleRange = imported.match(/\/ros2\/value\/data:3@(\d+)\.\.(\d+)=1\.25\.\.7\.75/);
  expect(sampleRange).not.toBeNull();
  const firstSampleTime = Number(sampleRange[1]) / 1e9;
  const lastSampleTime = Number(sampleRange[2]) / 1e9;
  expect(lastSampleTime).toBeGreaterThan(firstSampleTime);

  // Expand ros2-float64.mcap -> /ros2/value, then exercise the same real
  // CurveTreeView drag/drop path used by CSV. The leaf is the decoded ROS2
  // Float64 `data` field, not a probe-injected synthetic series.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);

  const readyMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_RHI_READY')) || '';
  expect(readyMessage).toContain('backend=OpenGLES2/WebGL');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '',
    { timeout: 15000 },
  ).toContain('first_curve=ros2/value/data');
  const frameMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '';
  expect(frameMessage).toContain('samples=3');
  expect(frameMessage).toContain('endpoints=0,1.25..2,7.75');

  let stateMessages = 0;
  let trackerMessages = 0;
  const curveMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).length;
  const initial = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(curveMessages);
  const curveReadback = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '';
  const curvePixels = curveReadback.match(/ros2\/value\/data:#[0-9a-f]{8}:(\d+)/i);
  expect(curvePixels).not.toBeNull();
  expect(Number(curvePixels[1])).toBeGreaterThan(0);
  await requestTrackerReadback(consoleMessages, trackerMessages);
  trackerMessages += 1;

  expect(initial.count).toBe(1);
  expect(initial.titles).toEqual(['ros2/value/data']);
  expect(initial.trackerEnabled).toBe(true);
  expect(initial.view.left).toBeLessThanOrEqual(firstSampleTime);
  expect(initial.view.right).toBeGreaterThanOrEqual(lastSampleTime);

  const canvas = {
    x: screen.x + initial.canvas.x,
    y: screen.y + initial.canvas.y,
    width: initial.canvas.width,
    height: initial.canvas.height,
  };
  const viewWidth = initial.view.right - initial.view.left;
  const seekRatio = 0.68;
  await seekQtPlot(page, canvas, 0.35, seekRatio);
  const sought = await requestPlotState(page, consoleMessages, stateMessages);
  const tracker = await requestTrackerReadback(consoleMessages, trackerMessages);
  const expectedTime = initial.view.left + (seekRatio * viewWidth);
  const timeTolerance = (3 * viewWidth / initial.canvas.width) + 1e-9;
  expect(Math.abs(sought.time - expectedTime)).toBeLessThan(timeTolerance);
  expect(sought.view).toEqual(initial.view);
  expect(tracker.pixels).toBeGreaterThan(0);
  expect(tracker.bounds.maxY - tracker.bounds.minY).toBeGreaterThan(0.85 * tracker.framebuffer.height);
  const trackerCenter = 0.5 * (tracker.bounds.minX + tracker.bounds.maxX);
  const trackerTolerance = Math.max(5 / tracker.framebuffer.width, 0.005);
  expect(Math.abs((trackerCenter / tracker.framebuffer.width) - seekRatio)).toBeLessThan(trackerTolerance);

  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('official MCAP indexless offset fetch renders and seeks stored scalars', async ({ page }) => {
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

  const fixture = unchunkedMcapFixture();
  expect(crypto.createHash('sha256').update(fixture).digest('hex')).toBe(
    '87d865a411c3f4caa977157e83ca0abb33c8835e0161ee1b22f16c58deda5fd3',
  );
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'unchunked.mcap',
    mimeType: 'application/octet-stream',
    buffer: fixture,
  });
  await page.waitForTimeout(750);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('/unchunked/seek/data:12@')) || '',
    { timeout: 30000 },
  ).toContain('=0..88');
  const imported = consoleMessages.find(message => message.includes('/unchunked/seek/data:12@')) || '';
  expect(imported).toContain('PJ_FILE_LOAD_OK plugin=MCAP Loader');
  expect(imported).toContain('scalar_series=/unchunked/seek/data');
  expect(imported).toContain('/unchunked/seek/data:12@0..2750000000=0..88');

  // This checked-in fixture is generated with use_chunking=False and
  // IndexType.NONE. A successful 12-payload decode therefore exercises the
  // plugin's indexless serial branch and its offset-backed fetchers during
  // eager scalar import. Seeking below reads the stored series; it does not
  // claim a second MCAP file read at tracker time.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);
  const readyMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_RHI_READY')) || '';
  expect(readyMessage).toContain('backend=OpenGLES2/WebGL');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '',
    { timeout: 15000 },
  ).toContain('first_curve=unchunked/seek/data');
  const frameMessage = consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '';
  expect(frameMessage).toContain('samples=12');
  expect(frameMessage).toContain('endpoints=0,0..2.75,88');

  let stateMessages = 0;
  let trackerMessages = 0;
  const curveMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).length;
  const initial = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(curveMessages);
  const curveReadback = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '';
  const curvePixels = curveReadback.match(/unchunked\/seek\/data:#[0-9a-f]{8}:(\d+)/i);
  expect(curvePixels).not.toBeNull();
  expect(Number(curvePixels[1])).toBeGreaterThan(0);
  await requestTrackerReadback(consoleMessages, trackerMessages);
  trackerMessages += 1;

  expect(initial.count).toBe(1);
  expect(initial.titles).toEqual(['unchunked/seek/data']);
  expect(initial.trackerEnabled).toBe(true);
  expect(initial.view.left).toBeLessThanOrEqual(0);
  expect(initial.view.right).toBeGreaterThanOrEqual(2.75);

  const canvas = {
    x: screen.x + initial.canvas.x,
    y: screen.y + initial.canvas.y,
    width: initial.canvas.width,
    height: initial.canvas.height,
  };
  const viewWidth = initial.view.right - initial.view.left;
  const seekRatio = 0.78;
  await seekQtPlot(page, canvas, 0.20, seekRatio);
  const sought = await requestPlotState(page, consoleMessages, stateMessages);
  const tracker = await requestTrackerReadback(consoleMessages, trackerMessages);
  const expectedTime = initial.view.left + (seekRatio * viewWidth);
  const timeTolerance = (3 * viewWidth / initial.canvas.width) + 1e-9;
  expect(Math.abs(sought.time - expectedTime)).toBeLessThan(timeTolerance);
  expect(sought.view).toEqual(initial.view);
  expect(tracker.pixels).toBeGreaterThan(0);
  expect(tracker.bounds.maxX - tracker.bounds.minX).toBeLessThanOrEqual(
    Math.max(4, 0.01 * tracker.framebuffer.width),
  );
  expect(tracker.bounds.maxY - tracker.bounds.minY).toBeGreaterThan(0.85 * tracker.framebuffer.height);
  const trackerCenter = 0.5 * (tracker.bounds.minX + tracker.bounds.maxX);
  const trackerTolerance = Math.max(5 / tracker.framebuffer.width, 0.005);
  expect(Math.abs((trackerCenter / tracker.framebuffer.width) - seekRatio)).toBeLessThan(trackerTolerance);

  expect(consoleMessages.some(
    message => /\[data_load_mcap\] lazy payload (open failed|record mismatch|short read)/.test(message),
  )).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PLOT_STATE_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('official ROS parser decodes ROS1 scalar messages from MCAP', async ({ page }) => {
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
    name: 'ros1-float64.mcap',
    mimeType: 'application/octet-stream',
    buffer: base64Fixture('ros1_float64.mcap.b64'),
  });
  await page.waitForTimeout(750);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(message => message.includes('/ros1/value/data:3@')) || '',
    { timeout: 30000 },
  ).toContain('=3.5..9');
  expect(errors).toEqual([]);
});
