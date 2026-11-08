// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const { waitForQtApp } = require('./app');
const { dragQtCanvas } = require('./canvas');
const { clickActiveDialogButton } = require('./dialogs');
const { openFileChooser } = require('./pickers');
const { qtPointToCss, requestScene3DFoundationState } = require('./scene3d_foundation');

const fixture = name => fs.readFileSync(path.resolve(__dirname, '..', 'fixtures', name));
const reportCount = messages => messages.filter(
  message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY'),
).length;

async function bootScene3D(page, messages) {
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('console', message => messages.push(message.text()));
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, messages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return screen;
}

async function loadScene3DFixture(page, screen, messages, name) {
  const loads = () => messages.filter(message => message.includes('PJ_FILE_LOAD_OK') && message.includes(`/${name}`)).length;
  const before = loads();
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name,
    mimeType: 'application/octet-stream',
    buffer: fixture(name),
  });
  await page.waitForTimeout(800);
  await clickActiveDialogButton(page, screen, messages, 'ok');
  // The dataset row the caller drags exists only once the import finishes, and a picked file is read on demand, so
  // how long that takes varies by browser and machine.
  await expect.poll(loads, { timeout: 60000 }).toBeGreaterThan(before);
  await page.waitForTimeout(500);
}

async function dropScene3DRows(page, screen, rows) {
  await page.mouse.click(screen.x + 10, screen.y + 192);
  const target = { x: screen.x + 820, y: screen.y + 390 };
  for (const y of rows) {
    await dragQtCanvas(page, { x: screen.x + 95, y: screen.y + y }, target);
    await page.waitForTimeout(300);
  }
}

async function waitForScene3DModels(page, messages, predicate, timeout = 30000) {
  let state;
  await expect.poll(async () => {
    state = await requestScene3DFoundationState(page, messages, reportCount(messages));
    return state.docks.length === 1 && predicate(state.docks[0].models, state.docks[0], state);
  }, { timeout }).toBe(true);
  return state;
}

async function clickScene3DControl(page, screen, state, control) {
  const center = qtPointToCss(screen, state, {
    x: control.x + control.width / 2,
    y: control.y + control.height / 2,
  });
  await page.mouse.click(center.x, center.y);
}

module.exports = {
  bootScene3D,
  clickScene3DControl,
  dropScene3DRows,
  fixture,
  loadScene3DFixture,
  waitForScene3DModels,
};
