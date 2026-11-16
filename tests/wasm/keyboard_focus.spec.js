// SPDX-License-Identifier: MPL-2.0
// Shortcuts must keep working after a press that no widget accepts, such as a
// rectangle zoom on a plot. Qt 6.12 lets the browser move focus to <body> then,
// and key events stop reaching Qt (QTBUG-144692).
const { test, expect } = require('@playwright/test');
const { waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { clickActiveDialogButton } = require('./support/dialogs');
const { firstLoadOutcome } = require('./support/load_outcomes');
const { openFileChooser } = require('./support/pickers');
const { requestPlotState } = require('./support/plot_probes');

test('Ctrl+Z and Space reach the app after a rectangle zoom on a plot', async ({ page }) => {
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
    name: 'focus.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp\n0,10\n1,20\n2,15\n3,30\n4,25\n5,20\n6,35\n7,15\n8,30\n9,25\n10,20\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(() => firstLoadOutcome(page), { timeout: 15000 }).toContain('scalar_series=focus/temp');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  await dragQtCanvas(page, { x: screen.x + 95, y: screen.y + 232 }, { x: screen.x + 820, y: screen.y + 390 });
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);

  let stateMessages = 0;
  const state = async () => requestPlotState(page, consoleMessages, stateMessages++);
  const initial = await state();
  const at = (fx, fy) => ({
    x: Math.round(screen.x + initial.canvas.x + (fx * initial.canvas.width)),
    y: Math.round(screen.y + initial.canvas.y + (fy * initial.canvas.height)),
  });

  const from = at(0.2, 0.2);
  const to = at(0.75, 0.75);
  await page.mouse.move(from.x, from.y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(to.x, to.y, { steps: 18 });
  await page.mouse.up({ button: 'left' });
  const zoomed = await state();
  expect(zoomed.view.right - zoomed.view.left).toBeLessThan(initial.view.right - initial.view.left);

  // A view change reaches the undo stack after a short debounce; an earlier undo would undo the step before it.
  await page.waitForTimeout(1000);
  await page.keyboard.press('Control+z');
  const width = initial.view.right - initial.view.left;
  await expect.poll(async () => {
    const { view } = await state();
    return ['left', 'right', 'bottom', 'top'].every(side => Math.abs(view[side] - initial.view[side]) < 1e-6 * width);
  }, { timeout: 10000 }).toBe(true);

  // A plain click on the plot is not accepted either; Space must still toggle playback.
  const center = at(0.5, 0.5);
  await page.mouse.click(center.x, center.y);
  const before = (await state()).time;
  await page.keyboard.press(' ');
  await expect.poll(async () => (await state()).time, { timeout: 10000 }).toBeGreaterThan(before);
  await page.keyboard.press(' ');

  expect(errors).toEqual([]);
});
