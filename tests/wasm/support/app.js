// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');
const { count, latest } = require('./console');

async function waitForQtApp(page, consoleMessages, previousReadyCount = 0) {
  await expect(page.locator('#screen')).toBeVisible({ timeout: 60000 });
  // Qt creates its HTML canvas before MainWindow is ready to receive input.
  // Wait for the first-show marker; the count makes this safe across reloads.
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_APP_READY')).length,
    { timeout: 60000 },
  ).toBeGreaterThan(previousReadyCount);
}


function observeRuntimeTarget(target, targetMessages, allMessages, errors) {
  // Firefox reports a worker thread's console line twice, from the worker and again from the page;
  // Chromium reports it once, from the worker. Keeping one copy lets specs count worker-side lines.
  const firefox = target.context().browser()?.browserType().name() === 'firefox';
  target.on('pageerror', error => errors.push(String(error)));
  target.on('console', (message) => {
    if (firefox && message.worker()) {
      return;
    }
    targetMessages.push(message.text());
    allMessages.push(message.text());
  });
}


// Opens the app in `page` and waits until it is ready. `messages` and `errors` collect the page's
// console log and uncaught errors; returns them with the screen's bounding box.
async function bootApp(page, messages = [], errors = []) {
  observeRuntimeTarget(page, messages, [], errors);
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, messages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return { messages, errors, screen };
}


async function replaceRuntimeTarget(currentPage, allMessages, errors, { hideOpenFilePicker = false } = {}) {
  const context = currentPage.context();
  await currentPage.close();
  const page = await context.newPage();
  if (hideOpenFilePicker) {
    await page.addInitScript(() => { delete window.showOpenFilePicker; });
  }
  const consoleMessages = [];
  observeRuntimeTarget(page, consoleMessages, allMessages, errors);
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return { page, consoleMessages, screen };
}

// The wasm heap: the bytes malloc holds, the linear memory's size (it never
// shrinks, so it is the peak so far) and the resident payload pool's capacity.
async function wasmHeap(page, messages) {
  const before = count(messages, 'PJ_WASM_HEAP in_use=');
  await page.evaluate(() => window.pjWasmReportHeapProbe());
  await expect.poll(() => count(messages, 'PJ_WASM_HEAP in_use=')).toBeGreaterThan(before);
  const [, inUse, memory, pool] = latest(messages, 'PJ_WASM_HEAP in_use=')
    .match(/in_use=(\d+) memory=(\d+) resident_pool=(\d+)/).map(Number);
  return { inUse, memory, pool };
}

module.exports = { bootApp, waitForQtApp, observeRuntimeTarget, replaceRuntimeTarget, wasmHeap };
