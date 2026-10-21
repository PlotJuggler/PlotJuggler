// SPDX-License-Identifier: MPL-2.0
// On-demand Python in a build with PJ_WASM_WITH_PYTHON. The probe (pj_app/tests/wasm_python_probe.inc) boots the
// interpreter from the main thread as the app's first compile does, then runs the checks on a worker. Without the
// option the spec skips, unless PJ_WASM_PYTHON=1 says the build must have it (WASM CI's acceptance package).
const { test, expect } = require('@playwright/test');
const { waitForQtApp } = require('./support/app');

// The watchdog's 2 s budget plus its poll and a loaded runner's scheduling.
const WATCHDOG_BOUND_MS = 2200;
// kPythonCompileWaitMillis (2 s) plus one 20 ms tick and scheduling. Slice P-c removes this wait.
const BUSY_COMPILE_GAP_BOUND_MS = 2200;

test('on-demand Python: stdlib, value, per-call cost, watchdog, strict profile, bounded main-thread waits', async ({
  page,
}) => {
  test.setTimeout(300_000);
  const consoleMessages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));
  // The longest gap between 20 ms ticks of the main thread's event loop, once armed.
  await page.addInitScript(() => {
    window.pjMainGap = { armed: false, max: 0, last: performance.now() };
    setInterval(() => {
      const now = performance.now();
      if (window.pjMainGap.armed) {
        window.pjMainGap.max = Math.max(window.pjMainGap.max, now - window.pjMainGap.last);
      }
      window.pjMainGap.last = now;
    }, 20);
  });
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);

  await page.evaluate(() => {
    window.pjMainGap.armed = true;
    window.pjMainGap.max = 0;
    window.pjMainGap.last = performance.now();
  });
  await page.evaluate(() => window.pjWasmPythonSmokeProbe());
  let hasPython = true;
  try {
    await expect.poll(() => consoleMessages.some(message => message.includes('PJ_WASM_PY_AVAILABLE')),
      { timeout: 10_000 }).toBe(true);
  } catch {
    hasPython = false;
  }
  test.skip(!hasPython && process.env.PJ_WASM_PYTHON !== '1', 'this build has no Python (PJ_WASM_WITH_PYTHON off)');
  expect(hasPython, 'PJ_WASM_PYTHON=1, but the build has no Python').toBe(true);
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PY_DONE')),
    { timeout: 240_000 },
  ).toBe(true);
  // A last tick after the worker finished, so a block at its very end is counted too.
  await page.waitForTimeout(200);
  const maxGap = await page.evaluate(() => window.pjMainGap.max);

  const lines = consoleMessages.filter(message => message.includes('PJ_WASM_PY_'));
  for (const line of lines) {
    console.log(line);
  }
  console.log(`PJ_WASM_PY main_thread_max_gap_ms=${maxGap.toFixed(1)}`);
  const line = marker => lines.find(message => message.includes(marker)) || '';
  const number = (marker, key) => Number((line(marker).match(new RegExp(`${key}=([-0-9.]+)`)) || [])[1]);

  expect(line('PJ_WASM_PY_AVAILABLE')).toContain('PJ_WASM_PY_AVAILABLE 1');
  expect(line('PJ_WASM_PY_BOOT')).toContain(' ok');
  // The allowed modules' lazy dependencies work inside a strict run (heapq, decimal's _pydecimal fallback), and every
  // other one a strict script can reach is loaded before any run.
  expect(number('PJ_WASM_PY_LAZY', 'value')).toBe(7);
  expect(line('PJ_WASM_PY_STDLIB_SURFACE').trim()).toBe('PJ_WASM_PY_STDLIB_SURFACE ok');
  expect(number('PJ_WASM_PY_VALUE', 'value')).toBeCloseTo(2 + Math.PI, 5);
  // Per call, against the same 4-line script in Luau in the same browser: an absolute bound would not hold across
  // browsers (Firefox runs this whole build 4-6x slower than Chromium, Luau included) or runners.
  const pythonMicros = number('PJ_WASM_PY_PERCALL four_line', 'four_line_mean_us');
  const luauMicros = number('PJ_WASM_PY_PERCALL luau', 'luau_four_line_mean_us');
  expect(pythonMicros).toBeGreaterThan(0);
  expect(pythonMicros).toBeLessThanOrEqual(luauMicros);
  const watchdog = line('PJ_WASM_PY_WATCHDOG');
  expect(watchdog).toContain('TimeoutError');
  expect(number('PJ_WASM_PY_WATCHDOG', 'elapsed_ms')).toBeLessThan(WATCHDOG_BOUND_MS);
  expect(line('PJ_WASM_PY_IMPORT_OS')).toContain("module 'os' is not available");
  expect(line('PJ_WASM_PY_PATCH_PI')).toContain('read-only');
  expect(line('PJ_WASM_PY_MEMCAP big')).toContain('over_budget=1');
  expect(line('PJ_WASM_PY_MEMCAP small')).toContain('over_budget=1');
  expect(maxGap).toBeLessThan(2000);
  expect(errors).toEqual([]);

  // Worst case: the GUI compiles while a worker script holds the GIL inside one C call; the wait is bounded by
  // kPythonCompileWaitMillis and the compile reports busy.
  await page.evaluate(() => window.pjWasmPythonHoldGilProbe());
  await page.waitForTimeout(500);
  await page.evaluate(() => {
    window.pjMainGap.max = 0;
    window.pjMainGap.last = performance.now();
  });
  await page.evaluate(() => window.pjWasmPythonCompileNowProbe());
  await page.waitForTimeout(100);
  const busyGap = await page.evaluate(() => window.pjMainGap.max);
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PY_HOLD_DONE')),
    { timeout: 120_000 },
  ).toBe(true);
  for (const message of consoleMessages.filter(m => m.includes('PJ_WASM_PY_BUSY') || m.includes('PJ_WASM_PY_HOLD'))) {
    console.log(message);
  }
  console.log(`PJ_WASM_PY busy_main_thread_max_gap_ms=${busyGap.toFixed(1)}`);
  expect(consoleMessages.find(m => m.includes('PJ_WASM_PY_BUSY_COMPILE'))).toContain('Python is busy');
  expect(busyGap).toBeLessThan(BUSY_COMPILE_GAP_BOUND_MS);
});
