// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const { bootApp } = require('./support/app');
const { COUNTER_AND_SINE, drawnPlots, robotPreset, startBridgeServer } = require('./support/bridge');
const { count, latest } = require('./support/console');
const { datasetCount, openPreset, startPresetServer, streamPreset } = require('./support/preset_server');

async function visibleDialogs(page, messages) {
  const before = count(messages, 'PJ_WASM_VISIBLE_DIALOGS count=');
  await page.evaluate(() => window.pjWasmReportVisibleDialogsProbe());
  await expect.poll(() => count(messages, 'PJ_WASM_VISIBLE_DIALOGS count=')).toBeGreaterThan(before);
  return latest(messages, 'PJ_WASM_VISIBLE_DIALOGS count=');
}

test('bridge preset loads without a dialog', async ({ page }) => {
  test.setTimeout(120000);
  const { messages, errors } = await bootApp(page);
  const bridge = await startBridgeServer();
  const presets = await startPresetServer({ 'robot/layout.pj4.xml': robotPreset(bridge.url, COUNTER_AND_SINE) });
  try {
    expect(await openPreset(page, messages, presets.url('robot/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    // The saved url is used verbatim, path included; the browser connects without asking.
    await expect.poll(() => bridge.seen('open').map(event => event.path), { timeout: 15000 }).toEqual(['/bridge']);
    await expect.poll(() => bridge.seen('request').map(event => event.command), { timeout: 15000 })
      .toContain('get_topics');
    expect(count(messages, 'PJ_WASM_SOURCE_LAYOUT_DECISION')).toBe(0);
    expect(await visibleDialogs(page, messages)).toContain('count=0');
    expect(errors).toEqual([]);
  } finally {
    await presets.close();
    bridge.close();
  }
});

test('compressed frames populate saved views', async ({ page }) => {
  test.setTimeout(120000);
  const { messages, errors } = await bootApp(page);
  const bridge = await startBridgeServer();
  const presets = await startPresetServer({ 'robot/layout.pj4.xml': robotPreset(bridge.url, COUNTER_AND_SINE) });
  try {
    expect(await openPreset(page, messages, presets.url('robot/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    // Each saved curve demands its topic. The tree and curve titles show bridge
    // topics without their leading slash.
    const drawnTitles = async () => (await drawnPlots(page, messages)).map(plot => plot.titles);
    await expect.poll(drawnTitles, { timeout: 30000 }).toEqual([['robot/counter/value'], ['robot/sine/value']]);
    // The topic no view shows is only previewed for its fields, then released.
    await expect.poll(() => bridge.subscribed(), { timeout: 15000 }).toEqual(['/robot/counter', '/robot/sine']);
    expect(await datasetCount(page, messages)).toBe(1);

    // The decompressed values reach their own plot: the counter keeps rising, the sine's view stays near [-1, 1].
    const [counter] = await drawnPlots(page, messages);
    await expect.poll(async () => (await drawnPlots(page, messages))[0].view.top, { timeout: 15000 })
      .toBeGreaterThan(counter.view.top);
    const [, sine] = await drawnPlots(page, messages);
    expect(sine.view.bottom).toBeGreaterThanOrEqual(-1.5);
    expect(sine.view.top).toBeLessThanOrEqual(1.5);
    expect(errors).toEqual([]);
  } finally {
    await presets.close();
    bridge.close();
  }
});

test('close during connection releases the socket', async ({ page, browserName }) => {
  test.setTimeout(120000);
  await page.addInitScript(() => {
    window.pjTestSockets = [];
    window.WebSocket = class extends window.WebSocket {
      constructor(...args) {
        super(...args);
        window.pjTestSockets.push(this);
      }
    };
  });
  const { messages, errors } = await bootApp(page);
  // The server reads the upgrade request and never answers: the browser socket stays connecting.
  const bridge = await startBridgeServer(['--hold-handshake']);
  const presets = await startPresetServer({
    'robot/layout.pj4.xml': robotPreset(bridge.url, COUNTER_AND_SINE),
    'next/layout.pj4.xml': streamPreset([{ id: 'next', config: '{}' }],
      [{ id: 'plot_next', curves: [['dummy/sawtooth', 'value', 'next']] }]),
  });
  try {
    expect(await openPreset(page, messages, presets.url('robot/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expect.poll(() => bridge.seen('connect').length, { timeout: 15000 }).toBe(1);
    const connected = Date.now();

    // The next preset replaces the connecting stream: stopping it closes the browser socket.
    const started = count(messages, 'Dummy streamer started');
    expect(await openPreset(page, messages, presets.url('next/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expect.poll(() => page.evaluate(() => window.pjTestSockets.map(socket => socket.readyState)), { timeout: 15000 })
      .toEqual([3]);  // WebSocket.CLOSED
    // Firefox fires the socket's close event at once but drops a connecting socket's TCP connection only
    // 10-20 s later, on its own schedule.
    if (browserName === 'chromium') {
      await expect.poll(() => bridge.seen('closed').length, { timeout: 15000 }).toBe(1);
    }
    await expect.poll(() => count(messages, 'Dummy streamer started'), { timeout: 15000 }).toBe(started + 1);
    expect(await datasetCount(page, messages)).toBe(1);

    // Past the plugin's 5 s connect deadline the stopped source stays silent: no timeout, no warning.
    await page.waitForTimeout(Math.max(0, connected + 6000 - Date.now()));
    expect(count(messages, 'PJ bridge at')).toBe(0);
    expect(await visibleDialogs(page, messages)).toContain('count=0');
    expect(errors).toEqual([]);
  } finally {
    await presets.close();
    bridge.close();
  }
});
