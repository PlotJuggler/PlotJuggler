// SPDX-License-Identifier: MPL-2.0
const fs = require('fs');
const { expect } = require('@playwright/test');

async function sourceLayoutDecisionGeometry(page, consoleMessages) {
  // MessageBox finalizes its wrapped-body height and re-centers in showEvent: sample button geometry after
  // that first layout pass.
  await page.waitForTimeout(500);
  const previousReselect = consoleMessages.filter(
    message => message.includes('PJ_WASM_SOURCE_LAYOUT_RESELECT center='),
  ).length;
  const previousCancel = consoleMessages.filter(
    message => message.includes('PJ_WASM_SOURCE_LAYOUT_CANCEL center='),
  ).length;
  await page.evaluate(() => window.pjWasmReportSourceLayoutDecisionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_SOURCE_LAYOUT_RESELECT center=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousReselect);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_SOURCE_LAYOUT_CANCEL center=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCancel);
  const parseCenter = (marker) => {
    const message = consoleMessages.filter(message => message.includes(marker)).at(-1) || '';
    const match = message.match(/center=(\d+),(\d+)/);
    expect(match, `unparseable source-layout decision geometry: ${message}`).not.toBeNull();
    return { x: Number(match[1]), y: Number(match[2]) };
  };
  return {
    reselect: parseCenter('PJ_WASM_SOURCE_LAYOUT_RESELECT center='),
    cancel: parseCenter('PJ_WASM_SOURCE_LAYOUT_CANCEL center='),
  };
}


async function downloadSourceLayoutFromFileMenu(page, screen, consoleMessages) {
  const fileMenuMessages = consoleMessages.filter(message => message.includes('PJ_WASM_FILE_MENU center=')).length;
  await page.evaluate(() => window.pjWasmReportFileMenuProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_FILE_MENU center=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(fileMenuMessages);
  const fileMenuMessage = consoleMessages.filter(message => message.includes('PJ_WASM_FILE_MENU center=')).at(-1) || '';
  const fileMenuCenter = fileMenuMessage.match(/center=(\d+),(\d+)/);
  expect(fileMenuCenter, `unparseable File menu geometry: ${fileMenuMessage}`).not.toBeNull();

  await page.mouse.click(screen.x + Number(fileMenuCenter[1]), screen.y + Number(fileMenuCenter[2]));
  await page.waitForTimeout(250);
  const actionMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=save_source_layout'),
  ).length;
  await page.evaluate(() => window.pjWasmReportSaveSourceLayoutActionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=save_source_layout')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(actionMessages);
  const actionMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=save_source_layout'),
  ).at(-1) || '';
  const actionCenter = actionMessage.match(/center=(\d+),(\d+)/);
  expect(actionCenter, `unparseable source-layout action geometry: ${actionMessage}`).not.toBeNull();

  const downloadRequests = consoleMessages.filter(
    message => message.includes('PJ_WASM_SOURCE_LAYOUT_DOWNLOAD_REQUESTED'),
  ).length;
  const downloadPromise = page.waitForEvent('download', { timeout: 15000 });
  await page.mouse.click(screen.x + Number(actionCenter[1]), screen.y + Number(actionCenter[2]));
  const download = await downloadPromise;
  const downloadPath = await download.path();
  expect(downloadPath).not.toBeNull();
  // The browser's download event and the worker's diagnostic reach Playwright on separate channels, in
  // either order.
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_SOURCE_LAYOUT_DOWNLOAD_REQUESTED'),
    ).length,
    { timeout: 10000 },
  ).toBeGreaterThan(downloadRequests);
  return {
    fileName: download.suggestedFilename(),
    bytes: fs.readFileSync(downloadPath),
  };
}


module.exports = { sourceLayoutDecisionGeometry, downloadSourceLayoutFromFileMenu };
