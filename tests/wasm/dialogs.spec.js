// SPDX-License-Identifier: MPL-2.0
// Confirmation dialogs, context menus and app dialogs on the Asyncify-free
// browser build: each must open non-blocking and act once the user answers.
const { test, expect } = require('@playwright/test');
const { waitForQtApp } = require('./support/app');
const { clickActiveDialogButton } = require('./support/dialogs');
const { curveRowCenter } = require('./support/geometry');
const { openFileChooser } = require('./support/pickers');

async function probeLines(page, consoleMessages, trigger, doneMarker) {
  const before = consoleMessages.length;
  await page.evaluate(trigger);
  await expect.poll(
    () => consoleMessages.slice(before).some(message => message.includes(doneMarker)),
    { timeout: 10000 },
  ).toBe(true);
  return consoleMessages.slice(before);
}

async function datasetCount(page, consoleMessages) {
  const lines = await probeLines(
    page, consoleMessages, () => window.pjWasmReportDatasetCountProbe(), 'PJ_WASM_DATASET_COUNT count=');
  const match = lines.find(message => message.includes('PJ_WASM_DATASET_COUNT count=')).match(/count=(\d+)/);
  return Number(match[1]);
}

async function menuButtonCenter(page, screen, consoleMessages, label) {
  const lines = await probeLines(
    page, consoleMessages, () => window.pjWasmReportMenuButtonsProbe(), 'PJ_WASM_MENU_BUTTONS_DONE');
  const match = lines
    .map(message => message.match(/PJ_WASM_MENU_BUTTON label=(.+) enabled=(\d) center=(-?\d+),(-?\d+)/))
    .find(entry => entry && entry[1] === label);
  expect(match, `menu button not visible: ${label}`).toBeTruthy();
  expect(match[2]).toBe('1');
  return { x: screen.x + Number(match[3]), y: screen.y + Number(match[4]) };
}

async function messageBox(page, screen, consoleMessages) {
  const lines = await probeLines(
    page, consoleMessages, () => window.pjWasmReportMessageBoxProbe(), 'PJ_WASM_MESSAGE_BOX_DONE');
  const done = lines.find(message => message.includes('PJ_WASM_MESSAGE_BOX_DONE'))
    .match(/present=(\d) title=(.*) count=(\d+)/);
  const buttons = new Map(lines
    .map(message => message.match(/PJ_WASM_MESSAGE_BOX_BUTTON label=(.+) center=(-?\d+),(-?\d+)/))
    .filter(Boolean)
    .map(entry => [entry[1], { x: screen.x + Number(entry[2]), y: screen.y + Number(entry[3]) }]));
  return { present: done[1] === '1', title: done[2], buttons };
}

async function loadCsv(page, screen, consoleMessages, name) {
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name,
    mimeType: 'text/csv',
    buffer: Buffer.from('time,value\n0,1\n1,2\n2,3\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length,
    { timeout: 15000 },
  ).toBeGreaterThan(0);
}

test('dataset removal asks without blocking and honours Cancel and Remove', async ({ page }) => {
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

  await loadCsv(page, screen, consoleMessages, 'removal.csv');
  expect(await datasetCount(page, consoleMessages)).toBe(1);

  const openRemovePrompt = async () => {
    const row = await curveRowCenter(page, screen, consoleMessages, 'removal.csv');
    await page.mouse.click(row.x, row.y, { button: 'right' });
    await page.waitForTimeout(300);
    const remove = await menuButtonCenter(page, screen, consoleMessages, 'Remove');
    await page.mouse.click(remove.x, remove.y);
    let box;
    await expect.poll(async () => {
      box = await messageBox(page, screen, consoleMessages);
      return box.present;
    }, { timeout: 10000 }).toBe(true);
    expect(box.title).toBe('Remove dataset');
    expect([...box.buttons.keys()]).toEqual(['Remove', 'Cancel']);
    return box;
  };

  // Cancel keeps the data: the question no longer reads as an instant "No".
  let box = await openRemovePrompt();
  await page.mouse.click(box.buttons.get('Cancel').x, box.buttons.get('Cancel').y);
  await expect.poll(async () => (await messageBox(page, screen, consoleMessages)).present).toBe(false);
  expect(await datasetCount(page, consoleMessages)).toBe(1);

  box = await openRemovePrompt();
  await page.mouse.click(box.buttons.get('Remove').x, box.buttons.get('Remove').y);
  await expect.poll(() => datasetCount(page, consoleMessages), { timeout: 10000 }).toBe(0);

  expect(errors).toEqual([]);
});

test('Preferences opens from the File menu without the Plugins page and closes on OK', async ({ page }) => {
  test.setTimeout(60000);
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const fileMenu = await probeLines(
    page, consoleMessages, () => window.pjWasmReportFileMenuProbe(), 'PJ_WASM_FILE_MENU');
  const fileCenter = fileMenu.find(message => message.includes('PJ_WASM_FILE_MENU center='))
    .match(/center=(\d+),(\d+)/);
  await page.mouse.click(screen.x + Number(fileCenter[1]), screen.y + Number(fileCenter[2]));
  await page.waitForTimeout(250);
  const action = await probeLines(
    page, consoleMessages, () => window.pjWasmReportPreferencesActionProbe(), 'PJ_WASM_MENU_ACTION');
  const actionMatch = action.find(message => message.includes('PJ_WASM_MENU_ACTION name=preferences'));
  expect(actionMatch, 'Preferences... is not in the browser File menu').toBeTruthy();
  const actionCenter = actionMatch.match(/center=(\d+),(\d+)/);
  await page.mouse.click(screen.x + Number(actionCenter[1]), screen.y + Number(actionCenter[2]));

  let report;
  await expect.poll(async () => {
    const lines = await probeLines(
      page, consoleMessages, () => window.pjWasmReportPreferencesDialogProbe(), 'PJ_WASM_PREFERENCES');
    report = lines.find(message => message.includes('PJ_WASM_PREFERENCES'))
      .match(/visible=(\d) ok=(-?\d+),(-?\d+) plugins_page=(\d)/);
    return report[1];
  }, { timeout: 10000 }).toBe('1');
  expect(report[4]).toBe('0');

  await page.mouse.click(screen.x + Number(report[2]), screen.y + Number(report[3]));
  await expect.poll(async () => {
    const lines = await probeLines(
      page, consoleMessages, () => window.pjWasmReportPreferencesDialogProbe(), 'PJ_WASM_PREFERENCES');
    return lines.find(message => message.includes('PJ_WASM_PREFERENCES')).match(/visible=(\d)/)[1];
  }, { timeout: 10000 }).toBe('0');

  expect(errors).toEqual([]);
});
