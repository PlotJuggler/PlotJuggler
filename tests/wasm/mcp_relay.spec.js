// SPDX-License-Identifier: MPL-2.0
// An AI agent drives the page through its relay (?mcp=): what only the browser
// build does differently from the desktop. The page cannot read the agent's
// disk, so files come from the relay, and it cannot block on a dialog.
const { test, expect } = require('@playwright/test');
const { observeRuntimeTarget, waitForQtApp } = require('./support/app');
const { startMcpRelay } = require('./support/mcp_relay');

// A CSV of `rows` samples whose value column is `scale` times the row number.
function csv(rows, scale) {
  const lines = ['time,value'];
  for (let row = 0; row < rows; row += 1) {
    lines.push(`${row},${row * scale}`);
  }
  return lines.join('\n') + '\n';
}

async function bootLinkedApp(page, relay) {
  const messages = [];
  const errors = [];
  observeRuntimeTarget(page, messages, [], errors);
  const base = process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html';
  await page.goto(`${base}${base.includes('?') ? '&' : '?'}mcp=${encodeURIComponent(relay.linkUrl)}`);
  await waitForQtApp(page, messages);
  await relay.pageJoined;
  return { messages, errors };
}

async function maxOf(relay, path) {
  const read = await relay.call('read_series', { paths: [path], mode: 'stats' });
  expect(read.isError, JSON.stringify(read.value)).toBe(false);
  return JSON.stringify(read.value);
}

test('reload_file downloads the relay file again and replaces the dataset in place', async ({ page }) => {
  const relay = await startMcpRelay();
  try {
    await bootLinkedApp(page, relay);
    const url = relay.serveFile('reading.csv', csv(50, 1));
    const opened = await relay.call('open_file', { url, name: 'reading.csv' });
    expect(opened.isError, JSON.stringify(opened.value)).toBe(false);
    const dataset = opened.value.dataset_id;
    expect(await maxOf(relay, 'reading/value')).toContain('"max":49');

    // The file changed on the agent's side: twice the samples, ten times the values.
    relay.serveFile('reading.csv', csv(100, 10));
    const reloaded = await relay.call('reload_file', { dataset });
    expect(reloaded.isError, JSON.stringify(reloaded.value)).toBe(false);
    expect(reloaded.value).toEqual({ reloaded: true, dataset_id: dataset });
    expect(await maxOf(relay, 'reading/value')).toContain('"max":990');
    const status = await relay.call('get_status');
    expect(status.value.datasets.map(entry => entry.id)).toEqual([dataset]);
  } finally {
    await relay.close();
  }
});

test('a stream dialog the agent asks for opens, and job_kill withdraws it', async ({ page }) => {
  const relay = await startMcpRelay();
  try {
    await bootLinkedApp(page, relay);
    // PlotJuggler Bridge has a configuration dialog: the call waits on the user, so it becomes a job.
    const asked = await relay.call('start_stream', { source: 'PlotJuggler Bridge', ask_user: true, wait_s: 1 });
    expect(asked.isError, JSON.stringify(asked.value)).toBe(false);
    expect(asked.value.state).toBe('waiting_for_user');
    const open = await relay.call('get_status');
    expect(open.value.user_dialog).not.toBeNull();

    const killed = await relay.call('job_kill', { job_id: asked.value.job_id });
    expect(killed.value.state).toBe('killed');
    const closed = await relay.call('get_status');
    expect(closed.value.user_dialog).toBeNull();
    expect(closed.value.datasets).toEqual([]);
  } finally {
    await relay.close();
  }
});
