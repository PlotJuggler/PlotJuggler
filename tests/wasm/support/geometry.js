// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

// Probe-driven canvas geometry: dataset rows and placeholder icons are addressed by name, not by pixel
// position, which moves with row pitch, header height and the icon set.

async function reportedLines(page, consoleMessages, trigger, marker) {
  const before = consoleMessages.length;
  await page.evaluate(trigger);
  await expect.poll(
    () => consoleMessages.slice(before).some(message => message.includes(marker)),
    { timeout: 5000 },
  ).toBe(true);
  return consoleMessages.slice(before);
}

// Center of a dataset-tree row addressed by its slash path (e.g.
// 'drag.csv/drag/value'). Expands the whole dataset tree as a side effect so
// the row is revealed before its geometry is reported.
async function curveRowCenter(page, screen, consoleMessages, path) {
  // The dataset tree populates after PJ_FILE_LOAD_OK, so retry the report until the row exists.
  let row;
  for (let attempt = 0; attempt < 8 && !row; ++attempt) {
    if (attempt > 0) {
      await page.waitForTimeout(500);
    }
    const lines = await reportedLines(
      page, consoleMessages, () => window.pjWasmReportCurveRowsProbe(), 'PJ_WASM_CURVE_ROWS_DONE');
    row = lines
      .map(message => message.match(/PJ_WASM_CURVE_ROW path=(.+) x=(-?\d+) y=(-?\d+) children=\d+/))
      .find(match => match && match[1] === path);
  }
  expect(row, `dataset row not reported by probe: ${path}`).toBeTruthy();
  return { x: screen.x + Number(row[2]), y: screen.y + Number(row[3]) };
}

// Center of one empty-dock placeholder family icon:
// 'plot' | 'transitions' | 'scene2d' | 'scene3d'.
async function placeholderIconCenter(page, screen, consoleMessages, kind) {
  const lines = await reportedLines(
    page, consoleMessages, () => window.pjWasmReportPlaceholderIconsProbe(), 'PJ_WASM_PLACEHOLDER_ICONS');
  const report = lines.find(message => message.includes('PJ_WASM_PLACEHOLDER_ICONS '));
  const match = report.match(new RegExp(`${kind}=(-?\\d+),(-?\\d+)`));
  expect(match, `placeholder icon not reported by probe: ${kind}`).toBeTruthy();
  return { x: screen.x + Number(match[1]), y: screen.y + Number(match[2]) };
}

module.exports = { curveRowCenter, placeholderIconCenter };
