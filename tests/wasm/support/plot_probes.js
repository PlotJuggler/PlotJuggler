// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

// Probe replies travel as console lines, so callers match them by `sequence=`
// and retry when a line is lost. Follow-up: a synchronous probe could return its
// report as the page.evaluate result (a C function returning a string), which
// would remove the sequence matching and the retries; a delayed readback would
// store its result for a later pull instead.

async function requestPlotState(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportPlotStateProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).at(-1) || '';
  const match = message.match(
    /count=(\d+) titles=(.*?) canvas=(\d+),(\d+),(\d+)x(\d+) view=([^,]+),([^,]+),([^,]+),([^ ]+) time=([^ ]+) tracker_enabled=(\d+) tracker_parameter=(\d+) xy=(\d+) style=(\d+) width=(\d+)/,
  );
  expect(match).not.toBeNull();
  return {
    count: Number(match[1]),
    titles: match[2] ? match[2].split(',') : [],
    canvas: {
      x: Number(match[3]),
      y: Number(match[4]),
      width: Number(match[5]),
      height: Number(match[6]),
    },
    view: {
      left: Number(match[7]),
      right: Number(match[8]),
      bottom: Number(match[9]),
      top: Number(match[10]),
    },
    time: Number(match[11]),
    trackerEnabled: match[12] === '1',
    trackerParameter: Number(match[13]),
    xy: match[14] === '1',
    style: Number(match[15]),
    width: Number(match[16]),
  };
}


async function requestMultiPlotState(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportMultiPlotStateProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MULTI_SUMMARY')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const summaryMessage = consoleMessages.filter(message => message.includes('PJ_WASM_MULTI_SUMMARY')).at(-1) || '';
  const summary = summaryMessage.match(/sequence=(\d+) docks=(\d+) plots=(\d+)/);
  expect(summary).not.toBeNull();
  const sequence = Number(summary[1]);
  const dockCount = Number(summary[2]);
  const plotCount = Number(summary[3]);
  const forSequence = (marker) => consoleMessages.filter(
    message => message.includes(marker) && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => forSequence('PJ_WASM_MULTI_DOCK').length, { timeout: 10000 }).toBe(dockCount);
  await expect.poll(() => forSequence('PJ_WASM_MULTI_PLOT').length, { timeout: 10000 }).toBe(plotCount);
  await expect.poll(() => forSequence('PJ_WASM_MULTI_READBACK').length, { timeout: 10000 }).toBe(plotCount);

  const docks = forSequence('PJ_WASM_MULTI_DOCK').map((message) => {
    const match = message.match(/index=(\d+) id=([^ ]+) kind=([^ ]+) rect=(\d+),(\d+),(\d+)x(\d+)/);
    expect(match, `unparseable multi-dock message: ${message}`).not.toBeNull();
    return {
      index: Number(match[1]),
      id: match[2],
      kind: match[3],
      rect: {
        x: Number(match[4]),
        y: Number(match[5]),
        width: Number(match[6]),
        height: Number(match[7]),
      },
    };
  }).sort((left, right) => left.index - right.index);

  const readbacksByPlot = new Map(forSequence('PJ_WASM_MULTI_READBACK').map((message) => {
    const match = message.match(
      /index=(\d+) plot_id=([^ ]+) curves=(.*?) tracker_pixels=(\d+) tracker_bounds=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) text_pixels=(\d+) text_bounds=(-?\d+),(-?\d+)\.\.(-?\d+),(-?\d+) framebuffer=(\d+)x(\d+)/,
    );
    expect(match, `unparseable multi-readback message: ${message}`).not.toBeNull();
    const curves = match[3] ? match[3].split(',').map((entry) => {
      const curve = entry.match(/^(.*):(#[0-9a-f]{8}):(\d+)$/i);
      expect(curve).not.toBeNull();
      return { title: curve[1], color: curve[2], pixels: Number(curve[3]) };
    }) : [];
    return [match[2], {
      curves,
      trackerPixels: Number(match[4]),
      trackerBounds: {
        minX: Number(match[5]),
        minY: Number(match[6]),
        maxX: Number(match[7]),
        maxY: Number(match[8]),
      },
      textPixels: Number(match[9]),
      textBounds: {
        minX: Number(match[10]),
        minY: Number(match[11]),
        maxX: Number(match[12]),
        maxY: Number(match[13]),
      },
      framebuffer: { width: Number(match[14]), height: Number(match[15]) },
    }];
  }));

  const plots = forSequence('PJ_WASM_MULTI_PLOT').map((message) => {
    const match = message.match(
      /index=(\d+) dock_id=([^ ]+) plot_id=([^ ]+) titles=(.*?) canvas=(\d+),(\d+),(\d+)x(\d+) view=([^,]+),([^,]+),([^,]+),([^ ]+) backend=([^ ]*) vertices=(\d+)/,
    );
    expect(match, `unparseable multi-plot message: ${message}`).not.toBeNull();
    return {
      index: Number(match[1]),
      dockId: match[2],
      plotId: match[3],
      titles: match[4] ? match[4].split(',') : [],
      canvas: {
        x: Number(match[5]),
        y: Number(match[6]),
        width: Number(match[7]),
        height: Number(match[8]),
      },
      view: {
        left: Number(match[9]),
        right: Number(match[10]),
        bottom: Number(match[11]),
        top: Number(match[12]),
      },
      backend: match[13],
      vertices: Number(match[14]),
      readback: readbacksByPlot.get(match[3]),
    };
  }).sort((left, right) => left.index - right.index);
  expect(plots.every(plot => plot.readback !== undefined)).toBe(true);
  return { sequence, docks, plots };
}

// Both replies carry the probe's sequence; the pixel readback is delayed and can
// land after a later request, so it is matched by sequence, not arrival order.
async function requestPlotHover(page, consoleMessages) {
  const previous = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER sequence=')).length;
  await page.evaluate(() => window.pjWasmReportPlotStateProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER sequence=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previous);
  const stateLine = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_HOVER sequence=')).at(-1);
  const sequence = stateLine.match(/sequence=(\d+) /)[1];
  const reply = prefix => consoleMessages.find(message => message.includes(`${prefix} sequence=${sequence} `)) || '';
  await expect.poll(() => reply('PJ_WASM_PLOT_HOVER'), { timeout: 10000 }).not.toBe('');
  await expect.poll(() => reply('PJ_WASM_PLOT_HOVER_PIXELS'), { timeout: 10000 }).not.toBe('');
  const stateMessage = reply('PJ_WASM_PLOT_HOVER');
  const stateMatch = stateMessage.match(
    /enabled=(\d+) visible=(\d+) point=([^,]+),([^ ]+) label=([^\s]*)/,
  );
  expect(stateMatch, `unparseable plot-hover state: ${stateMessage}`).not.toBeNull();
  const pixelMessage = reply('PJ_WASM_PLOT_HOVER_PIXELS');
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

module.exports = { requestPlotState, requestMultiPlotState, requestPlotHover };
