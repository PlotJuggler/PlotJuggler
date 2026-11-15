// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

// Canvas-relative centers of the active dialog's OK and Cancel buttons; -1,-1 for one it lacks, or with no
// dialog open. The probe runs synchronously and leaves its report on the page (Firefox can drop the console line).
async function activeDialogButtonGeometry(page) {
  const report = await page.evaluate(() => {
    window.pjWasmReportDialogButtonsProbe();
    return globalThis.pjWasmDialogButtons || '';
  });
  const match = report.match(/ok=(-?\d+),(-?\d+) cancel=(-?\d+),(-?\d+)/);
  return {
    ok: { x: match ? Number(match[1]) : -1, y: match ? Number(match[2]) : -1 },
    cancel: { x: match ? Number(match[3]) : -1, y: match ? Number(match[4]) : -1 },
  };
}


// `consoleMessages` is unused, kept so callers need not change.
async function clickActiveDialogButton(page, screen, consoleMessages, button) {
  // Canonical host chrome may legitimately change the dialog's size and
  // centering. Query Qt for the live canvas-global button center, then retain a
  // physical Playwright mouse click through the same production event path.
  await page.waitForTimeout(250);
  const geometry = await activeDialogButtonGeometry(page);
  const point = geometry[button];
  expect(point.x, `${button} button is absent from the active dialog`).toBeGreaterThanOrEqual(0);
  expect(point.y, `${button} button is absent from the active dialog`).toBeGreaterThanOrEqual(0);
  await page.mouse.click(screen.x + point.x, screen.y + point.y);
}


module.exports = { activeDialogButtonGeometry, clickActiveDialogButton };
