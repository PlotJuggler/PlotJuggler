// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');
const { count, latest, layoutOutcome } = require('./console');
const { startSecondOrigin } = require('./second_origin');


// Hosts preset folders under /presets/ on a real second origin, so every
// download crosses the browser's CORS checks like a preset on its own host.
// `files` maps a path below /presets/ to its body, or to a second_origin route
// ({ body, cors = true, hold, headers }). url(path) names a hosted (or missing) path.
async function startPresetServer(files) {
  const routes = Object.fromEntries(Object.entries(files).map(([path, file]) => [
    `/presets/${path}`,
    Buffer.isBuffer(file) || typeof file === 'string' ? { body: file } : file,
  ]));
  const server = await startSecondOrigin(routes);
  return { ...server, url: path => `${server.origin}/presets/${path}` };
}

// Opens `url` the way a preset does and returns its PJ_WASM_LAYOUT_LOAD_* outcome.
async function openPreset(page, messages, url) {
  const before = messages.length;
  await page.evaluate(target => window.pjWasmLoadLayoutUrlProbe(target), url);
  const outcome = () => layoutOutcome(messages, url, before);
  await expect.poll(outcome, { timeout: 30000 }).not.toBe('');
  return outcome();
}

// Closes every warning a failure raised (the replay's and FileLoader's own).
async function dismissDialogs(page, messages) {
  for (let attempt = 0; attempt < 5; attempt += 1) {
    const before = count(messages, 'PJ_WASM_LAYOUT_DIALOG present=');
    await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
    await expect.poll(() => count(messages, 'PJ_WASM_LAYOUT_DIALOG present=')).toBeGreaterThan(before);
    if (latest(messages, 'PJ_WASM_LAYOUT_DIALOG present=').includes('present=0')) {
      return;
    }
    await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());
    await page.waitForTimeout(200);
  }
  throw new Error('a warning dialog would not close');
}

// The number of datasets the catalog lists.
async function datasetCount(page, messages) {
  const before = count(messages, 'PJ_WASM_DATASET_COUNT count=');
  await page.evaluate(() => window.pjWasmReportDatasetCountProbe());
  await expect.poll(() => count(messages, 'PJ_WASM_DATASET_COUNT count=')).toBeGreaterThan(before);
  return Number(latest(messages, 'PJ_WASM_DATASET_COUNT count=').match(/count=(\d+)/)[1]);
}

const DUMMY_PLUGIN = '<plugin ID="Dummy Streamer" manifest_id="dummy-streamer">';

// A preset layout with only streaming sources: one plot per entry of `plots`,
// whose curves ([topic, field, stream id]) name the stream they are drawn from.
// A stream's `plugin` is its opening <plugin> tag (the Dummy Streamer when omitted).
function streamPreset(streams, plots) {
  const areas = plots.map(({ id, curves }) => `<DockArea id="${id}-area" name="${id}"><plot id="${id}" `
    + `mode="TimeSeries">${curves.map(([topic, field, stream]) => `<curve topic="${topic}" field="${field}" `
    + `dataset_path="pj-stream://${stream}"/>`).join('')}</plot></DockArea>`).join('');
  const records = streams.map(({ id, config, plugin = DUMMY_PLUGIN }) =>
    `<streamInfo id="${id}">${plugin}<![CDATA[${config}]]></plugin></streamInfo>`).join('');
  return '<root pj4_version="5" binding="source"><tabbed_widget parent="main_window"><Tab id="t1" containers="1">'
    + `<Container><DockSplitter count="${plots.length}" orientation="-">${areas}</DockSplitter></Container>`
    + `</Tab></tabbed_widget><previouslyLoaded_StreamingSources>${records}</previouslyLoaded_StreamingSources></root>`;
}

module.exports = { datasetCount, dismissDialogs, openPreset, startPresetServer, streamPreset };
