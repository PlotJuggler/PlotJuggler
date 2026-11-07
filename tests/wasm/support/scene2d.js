// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

// Asks the acceptance build for its Scene2D docks (layers, viewer geometry,
// colour readback) and the timeline slider, and returns them parsed.
async function requestScene2DState(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportScene2DStateProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_SCENE2D_SUMMARY')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const summaryMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE2D_SUMMARY'),
  ).at(-1) || '';
  const summary = summaryMessage.match(
    /sequence=(\d+) docks=(\d+) slider=(-?\d+),(-?\d+),(\d+)x(\d+) value=([^ ]+) range=([^,]+),([^ ]+) enabled=(\d+)/,
  );
  expect(summary, `unparseable Scene2D summary: ${summaryMessage}`).not.toBeNull();
  const sequence = Number(summary[1]);
  const dockCount = Number(summary[2]);
  const forSequence = marker => consoleMessages.filter(
    message => message.includes(marker) && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => forSequence('PJ_WASM_SCENE2D_DOCK').length).toBe(dockCount);
  await expect.poll(() => forSequence('PJ_WASM_SCENE2D_READBACK').length).toBe(dockCount);

  const readbacks = new Map(forSequence('PJ_WASM_SCENE2D_READBACK').map((message) => {
    const match = message.match(
      /index=(\d+) red=(\d+) blue=(\d+) green=(\d+) magenta=(\d+) yellow=(\d+) cyan=(\d+) white=(\d+) framebuffer=(\d+)x(\d+)/,
    );
    expect(match, `unparseable Scene2D readback: ${message}`).not.toBeNull();
    return [Number(match[1]), {
      red: Number(match[2]),
      blue: Number(match[3]),
      green: Number(match[4]),
      magenta: Number(match[5]),
      yellow: Number(match[6]),
      cyan: Number(match[7]),
      white: Number(match[8]),
      framebuffer: { width: Number(match[9]), height: Number(match[10]) },
    }];
  }));
  const docks = forSequence('PJ_WASM_SCENE2D_DOCK').map((message) => {
    const match = message.match(
      /index=(\d+) identity=(\d+) layers=(\d+) viewer=(-?\d+),(-?\d+),(\d+)x(\d+) visible=(\d+) view=([^,]+),([^,]+),([^ ]+)/,
    );
    expect(match, `unparseable Scene2D dock: ${message}`).not.toBeNull();
    return {
      index: Number(match[1]),
      identity: Number(match[2]),
      layers: Number(match[3]),
      viewer: {
        x: Number(match[4]),
        y: Number(match[5]),
        width: Number(match[6]),
        height: Number(match[7]),
      },
      visible: match[8] === '1',
      view: { zoom: Number(match[9]), panX: Number(match[10]), panY: Number(match[11]) },
      readback: readbacks.get(Number(match[1])),
    };
  }).sort((left, right) => left.index - right.index);

  return {
    sequence,
    docks,
    slider: {
      x: Number(summary[3]),
      y: Number(summary[4]),
      width: Number(summary[5]),
      height: Number(summary[6]),
      value: Number(summary[7]),
      min: Number(summary[8]),
      max: Number(summary[9]),
      enabled: summary[10] === '1',
    },
  };
}

module.exports = { requestScene2DState };
