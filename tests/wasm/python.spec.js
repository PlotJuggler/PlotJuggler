// SPDX-License-Identifier: MPL-2.0
// On-demand Python in a build with PJ_WASM_WITH_PYTHON. The probe (pj_app/tests/wasm_python_probe.inc) makes the
// main thread's compile as the app's GUI does (no interpreter touched), then boots the interpreter and runs the checks
// on a worker. Without the option the spec skips, unless PJ_WASM_PYTHON=1 says the build must have it (WASM CI's
// acceptance package).
const { test, expect } = require('@playwright/test');
const { waitForQtApp } = require('./support/app');

// The watchdog's 2 s budget plus its poll and a loaded runner's scheduling.
const WATCHDOG_BOUND_MS = 2200;
// A GUI compile never waits for the GIL (slice P-c; it waited up to 2 s before), and a validate waits at most its
// 100 ms bound (kPythonValidateWaitMillis): that, one 20 ms tick and a loaded runner's scheduling.
const BUSY_COMPILE_GAP_BOUND_MS = 250;
// Python per call against the same script in Luau. Firefox's ratio depends on the CPU core it lands on (on a hybrid
// laptop 1.2x on efficiency cores, 1.55x on performance cores; 0.85x on CI), Chromium's does not (1.0-1.26x).
const PER_CALL_RATIO_BOUND = { chromium: 1.5, firefox: 2 };
const STDLIB_ASSET = '/python_stdlib.zip';

// @serial: its time bounds fail on a loaded runner (playwright.config.js).
test('on-demand Python: stdlib, value, per-call cost, watchdog, strict profile, no main-thread wait', {
  tag: '@serial',
}, async ({ page, browserName }) => {
  test.setTimeout(300_000);
  const consoleMessages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));
  // The stdlib is a separate asset (scripts/package_wasm.mjs) the first Python use downloads, not the startup.
  const stdlibRequests = [];
  page.context().on('request', request => {
    if (request.url().endsWith(STDLIB_ASSET)) {
      stdlibRequests.push(request.url());
    }
  });
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
  expect(stdlibRequests, 'the app starts without the Python stdlib').toEqual([]);

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
  // Not in the filesystem before the first use, there after it. Playwright sees a worker's requests in Chromium only.
  expect(line('PJ_WASM_PY_STDLIB_BEFORE')).toContain('stat=-1');
  expect(number('PJ_WASM_PY_STDLIB ', 'bytes')).toBeGreaterThan(1_000_000);
  if (browserName === 'chromium') {
    expect(stdlibRequests, 'the first Python use downloads the stdlib, once').toHaveLength(1);
  }
  // The allowed modules' lazy dependencies work inside a strict run (heapq, decimal's _pydecimal fallback), and every
  // other one a strict script can reach is loaded before any run.
  expect(number('PJ_WASM_PY_LAZY', 'value')).toBe(7);
  expect(line('PJ_WASM_PY_STDLIB_SURFACE').trim()).toBe('PJ_WASM_PY_STDLIB_SURFACE ok');
  expect(number('PJ_WASM_PY_VALUE', 'value')).toBeCloseTo(2 + Math.PI, 5);
  // Per call, against the same 4-line script in Luau in the same browser, timed in alternating rounds: an absolute
  // bound would not hold across browsers or runners. Luau reuses its sandboxed state per thread, so its call is cheap,
  // and staying near it catches a Python regression. Every timed call must have succeeded, or the timing means nothing.
  const pythonMicros = number('PJ_WASM_PY_PERCALL four_line', 'four_line_mean_us');
  const luauMicros = number('PJ_WASM_PY_PERCALL luau', 'luau_four_line_mean_us');
  expect(number('PJ_WASM_PY_PERCALL four_line', 'failed')).toBe(0);
  expect(number('PJ_WASM_PY_PERCALL four_line', 'value')).toBeCloseTo(Math.sqrt(10), 5);
  expect(number('PJ_WASM_PY_PERCALL luau', 'value')).toBeCloseTo(Math.sqrt(10), 5);
  expect(pythonMicros).toBeGreaterThan(0);
  expect(pythonMicros).toBeLessThanOrEqual(PER_CALL_RATIO_BOUND[browserName] * luauMicros);
  // Allocation-heavy code is measured, not bounded (ARCHITECTURE §14 quotes it per browser).
  expect(number('PJ_WASM_PY_ALLOC sum_range', 'value')).toBe(499999500000);
  expect(number('PJ_WASM_PY_ALLOC sum_range', 'ns_per_element')).toBeGreaterThan(0);
  const watchdog = line('PJ_WASM_PY_WATCHDOG');
  expect(watchdog).toContain('TimeoutError');
  expect(number('PJ_WASM_PY_WATCHDOG', 'elapsed_ms')).toBeLessThan(WATCHDOG_BOUND_MS);
  expect(line('PJ_WASM_PY_IMPORT_OS')).toContain("module 'os' is not available");
  expect(line('PJ_WASM_PY_PATCH_PI')).toContain('read-only');
  expect(line('PJ_WASM_PY_MEMCAP big')).toContain('over_budget=1');
  expect(line('PJ_WASM_PY_MEMCAP small')).toContain('over_budget=1');
  expect(maxGap).toBeLessThan(2000);
  expect(errors).toEqual([]);

  // Worst case: the GUI compiles and validates while a worker script holds the GIL inside one C call. The compile
  // touches no interpreter, so it returns at once; the validate waits its bound and reports the script unchecked; the
  // script then compiles and runs on a worker once the holder ends. Before the holder starts, a validate of the same
  // broken script, with the interpreter free, reports its syntax error.
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
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PY_BUSY_RUN')),
    { timeout: 120_000 },
  ).toBe(true);
  for (const message of consoleMessages.filter(
    m => m.includes('PJ_WASM_PY_BUSY') || m.includes('PJ_WASM_PY_HOLD') || m.includes('PJ_WASM_PY_FREE'))) {
    console.log(message);
  }
  console.log(`PJ_WASM_PY busy_main_thread_max_gap_ms=${busyGap.toFixed(1)}`);
  expect(consoleMessages.find(m => m.includes('PJ_WASM_PY_BUSY_COMPILE'))).toContain(' ok');
  expect(consoleMessages.find(m => m.includes('PJ_WASM_PY_BUSY_VALIDATE'))).toContain(' unchecked ');
  expect(consoleMessages.find(m => m.includes('PJ_WASM_PY_FREE_VALIDATE'))).toContain('SyntaxError');
  expect(Number((consoleMessages.find(m => m.includes('PJ_WASM_PY_BUSY_RUN')).match(/value=([-0-9.]+)/) || [])[1]))
    .toBeCloseTo(Math.sqrt(10), 5);
  expect(busyGap).toBeLessThan(BUSY_COMPILE_GAP_BOUND_MS);
});

// The stdlib download fails, before any Python use succeeded: each failure is reported as a download failure (a
// validate leaves a valid script unchecked, not invalid), none sticks to the recipe's handle, and the same handle runs
// once the download works. A failure is not retried before its cooldown (shortened here), so a playback re-running a
// script every frame does not re-download every frame; a 404 (the app was redeployed) is never retried. Playwright
// routes a worker's requests in Chromium only.
test('on-demand Python: a failed stdlib download is reported and retried, never kept', async ({ page, browserName }) => {
  test.skip(browserName !== 'chromium', "Playwright routes a worker's requests in Chromium only");
  test.setTimeout(300_000);
  const COOLDOWN_MS = 2000;
  const consoleMessages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));
  let requests = 0;
  let answer = { status: 503, body: 'unavailable' };
  await page.context().route(`**${STDLIB_ASSET}`, route => {
    requests += 1;
    return answer ? route.fulfill(answer) : route.continue();
  });
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const lines = marker => consoleMessages.filter(message => message.includes(marker));
  const probe = (step, arg = 0) => page.evaluate(([s, a]) => window.pjWasmPythonStdlibProbe(s, a), [step, arg]);
  const logged = async (marker, previous) => {
    await expect.poll(() => lines(marker).length, { timeout: 120_000 }).toBeGreaterThan(previous);
    return lines(marker).at(-1);
  };
  const handle = async () => {
    const before = lines('PJ_WASM_PY_DL_HANDLE').length;
    await probe(0);
    await probe(3, COOLDOWN_MS);
    const available = await expect.poll(() => lines('PJ_WASM_PY_DL_HANDLE').length, { timeout: 10_000 })
      .toBeGreaterThan(before).then(() => true, () => false);
    test.skip(!available && process.env.PJ_WASM_PYTHON !== '1', 'this build has no Python (PJ_WASM_WITH_PYTHON off)');
    expect(lines('PJ_WASM_PY_DL_HANDLE').at(-1)).toContain(' ok');
  };
  let attempts = 0;
  const run = async () => {
    attempts += 1;
    await probe(2, attempts);
    return logged(`PJ_WASM_PY_DL_RUN ${attempts} `, 0);
  };
  const afterCooldown = () => page.waitForTimeout(COOLDOWN_MS + 500);

  await handle();
  // Soft: every step reports, so one run shows each failure.
  await probe(1, 30_000);
  const validate = await logged('PJ_WASM_PY_DL_VALIDATE', 0);
  expect.soft(validate).toContain('unchecked');
  expect.soft(validate).toContain('could not download its standard library');
  expect.soft(await run()).toContain('could not download its standard library (HTTP 503)');
  expect.soft(await run()).toContain('could not download its standard library (HTTP 503)');
  expect.soft(requests, 'no new request within the cooldown').toBe(1);

  await afterCooldown();
  answer = { status: 200, contentType: 'text/html', body: '<html><body>a login page</body></html>' };
  expect.soft(await run()).toContain('could not download its standard library (not a zip archive)');
  expect.soft(requests).toBe(2);
  await afterCooldown();
  answer = null;
  expect.soft(await run()).toMatch(/value=5\.14159/);
  expect.soft(requests).toBe(3);

  // A fresh page, the stdlib not downloaded yet: a 404 holds for the session, whatever the cooldown.
  requests = 0;
  answer = { status: 404, body: 'not found' };
  const ready = lines('PJ_WASM_APP_READY').length;
  await page.reload();
  await waitForQtApp(page, consoleMessages, ready);
  await handle();
  expect.soft(await run()).toContain('the app was updated since this page was loaded; reload the page');
  expect.soft(await run()).toContain('the app was updated since this page was loaded; reload the page');
  await afterCooldown();
  expect.soft(await run()).toContain('the app was updated since this page was loaded; reload the page');
  expect.soft(requests, 'a 404 is requested once').toBe(1);
  expect(errors).toEqual([]);
});
