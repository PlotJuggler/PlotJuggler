// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const { bootApp, waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { count, latest } = require('./support/console');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { requestMultiPlotState } = require('./support/plot_probes');
const {
  datasetCount, dismissDialogs, openPreset, startPresetServer, streamPreset,
} = require('./support/preset_server');
const { downloadSourceLayoutFromFileMenu } = require('./support/source_layout');

function decodeBase64(value) {
  return value ? Buffer.from(value, 'base64').toString('utf8') : '';
}

function point(x, y) {
  return { x: Number(x), y: Number(y) };
}

const STREAM_REPORT_MARKERS = [
  'PJ_WASM_STREAM_STATE sequence=',
  'PJ_WASM_STREAM_CONTROLS sequence=',
  'PJ_WASM_STREAM_TREE sequence=',
  'PJ_WASM_STREAM_CURVE sequence=',
];

// One probe call prints four console lines tagged with the same sequence.
// Firefox occasionally never delivers one of them (seen on the P8 build too:
// a main-thread line lost while the streamer worker logs at the same moment);
// the app stays responsive and the next call reports normally. A report whose
// four lines do not all arrive is requested again, so one lost console line
// cannot fail the test; the state checks themselves are unchanged.
async function awaitStreamingReport(page, consoleMessages) {
  let report;
  await expect(async () => {
    const first = consoleMessages.length;
    await page.evaluate(() => window.pjWasmReportStreamingStateProbe());
    await expect.poll(() => {
      const bySequence = new Map();
      for (const message of consoleMessages.slice(first)) {
        const marker = STREAM_REPORT_MARKERS.find(candidate => message.includes(candidate));
        const sequence = marker && message.match(/sequence=(\d+) /);
        if (sequence) {
          bySequence.set(sequence[1], (bySequence.get(sequence[1]) || new Map()).set(marker, message));
        }
      }
      const complete = [...bySequence].find(([, lines]) => lines.size === STREAM_REPORT_MARKERS.length);
      report = complete && { sequence: Number(complete[0]), lines: complete[1] };
      return report !== undefined;
    }, { timeout: 3500 }).toBe(true);
  }, 'streaming state probe produced no complete report').toPass({ timeout: 12000 });
  return report;
}

async function requestStreamingState(page, consoleMessages) {
  const report = await awaitStreamingReport(page, consoleMessages);
  const stateMessage = report.lines.get('PJ_WASM_STREAM_STATE sequence=');
  const state = stateMessage.match(
    /sequence=(\d+) sources=(\d+) source_b64=([^ ]*) active=(\d+) datasets=(\d+) catalog=(\d+) paused=(\d+) playing=(\d+) range=([^,]+),([^ ]+) current=([^ ]+)/,
  );
  expect(state, `unparseable stream state: ${stateMessage}`).not.toBeNull();
  const sequence = report.sequence;
  const messageFor = marker => report.lines.get(marker);

  const controlsMessage = messageFor('PJ_WASM_STREAM_CONTROLS sequence=');
  const controls = controlsMessage.match(
    /tab=(-?\d+),(-?\d+) combo=(-?\d+),(-?\d+) start=(-?\d+),(-?\d+) pause=(-?\d+),(-?\d+) buffer=(-?\d+),(-?\d+) tree=(-?\d+),(-?\d+) menu=(-?\d+),(-?\d+) remove=(-?\d+),(-?\d+) canvas=(-?\d+),(-?\d+)/,
  );
  expect(controls, `unparseable stream controls: ${controlsMessage}`).not.toBeNull();

  const treeMessage = messageFor('PJ_WASM_STREAM_TREE sequence=');
  const tree = treeMessage.match(
    /rows=(\d+) dataset_b64=([^ ]*) dataset=(-?\d+),(-?\d+) topic_b64=([^ ]*) topic=(-?\d+),(-?\d+) field_b64=([^ ]*) field=(-?\d+),(-?\d+)/,
  );
  expect(tree, `unparseable stream tree: ${treeMessage}`).not.toBeNull();

  const curveMessage = messageFor('PJ_WASM_STREAM_CURVE sequence=');
  const curve = curveMessage.match(
    /plotted=(\d+) title_b64=([^ ]*) samples=(\d+) first=([^,]+),([^ ]+) last=([^,]+),([^ ]+) vertices=(\d+)/,
  );
  expect(curve, `unparseable stream curve: ${curveMessage}`).not.toBeNull();

  return {
    sequence,
    sources: Number(state[2]),
    source: decodeBase64(state[3]),
    active: state[4] === '1',
    datasets: Number(state[5]),
    catalog: Number(state[6]),
    paused: state[7] === '1',
    playing: state[8] === '1',
    range: { min: Number(state[9]), max: Number(state[10]) },
    current: Number(state[11]),
    controls: {
      tab: point(controls[1], controls[2]),
      combo: point(controls[3], controls[4]),
      start: point(controls[5], controls[6]),
      pause: point(controls[7], controls[8]),
      buffer: point(controls[9], controls[10]),
      tree: point(controls[11], controls[12]),
      menu: point(controls[13], controls[14]),
      remove: point(controls[15], controls[16]),
      canvas: point(controls[17], controls[18]),
    },
    tree: {
      rows: Number(tree[1]),
      datasetName: decodeBase64(tree[2]),
      dataset: point(tree[3], tree[4]),
      topicName: decodeBase64(tree[5]),
      topic: point(tree[6], tree[7]),
      fieldName: decodeBase64(tree[8]),
      field: point(tree[9], tree[10]),
    },
    curve: {
      plotted: curve[1] === '1',
      title: decodeBase64(curve[2]),
      samples: Number(curve[3]),
      first: { x: Number(curve[4]), y: Number(curve[5]) },
      last: { x: Number(curve[6]), y: Number(curve[7]) },
      vertices: Number(curve[8]),
    },
  };
}

async function waitForStreamingState(page, consoleMessages, predicate, timeout = 20000) {
  let latest;
  await expect.poll(async () => {
    latest = await requestStreamingState(page, consoleMessages);
    return predicate(latest);
  }, { timeout }).toBe(true);
  return latest;
}

async function clickQt(page, screen, position, clickCount = 1) {
  expect(position.x).toBeGreaterThanOrEqual(0);
  expect(position.y).toBeGreaterThanOrEqual(0);
  const absolute = { x: screen.x + position.x, y: screen.y + position.y };
  if (clickCount === 2) {
    await page.mouse.dblclick(absolute.x, absolute.y, { delay: 80 });
  } else {
    await page.mouse.click(absolute.x, absolute.y);
  }
}

async function startStreamWithPhysicalInput(page, screen, consoleMessages, initialState) {
  let state = initialState;
  for (let attempt = 0; attempt < 5 && !state.active; ++attempt) {
    await clickQt(page, screen, state.controls.start);
    await page.waitForTimeout(500);
    state = await requestStreamingState(page, consoleMessages);
  }
  expect(state.active, 'the real Start streaming button did not activate after bounded retries').toBe(true);
  return state;
}

async function assertPlotReadback(page, consoleMessages) {
  const states = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  const readbacks = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS curves=')).length;
  await page.evaluate(() => window.pjWasmReportPlotStateProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(states);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS curves=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(readbacks);
  const state = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).at(-1) || '';
  expect(state).toContain('count=1');
  const pixels = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS curves=')).at(-1) || '';
  const counts = [...pixels.matchAll(/#[0-9a-f]{8}:(\d+)/ig)].map(match => Number(match[1]));
  expect(counts.length, `unparseable plot readback: ${pixels}`).toBeGreaterThan(0);
  expect(Math.max(...counts), `curve produced no framebuffer pixels: ${pixels}`).toBeGreaterThan(0);
}

async function fileMenuPoint(page, consoleMessages) {
  const previous = consoleMessages.filter(message => message.includes('PJ_WASM_FILE_MENU center=')).length;
  await page.evaluate(() => window.pjWasmReportFileMenuProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_FILE_MENU center=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previous);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_FILE_MENU center=')).at(-1) || '';
  const match = message.match(/center=(\d+),(\d+)/);
  expect(match, `unparseable File menu point: ${message}`).not.toBeNull();
  return point(match[1], match[2]);
}

async function quitActionPoint(page, consoleMessages) {
  const previous = consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=quit')).length;
  await page.evaluate(() => window.pjWasmReportQuitActionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=quit')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previous);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=quit')).at(-1) || '';
  const match = message.match(/center=(\d+),(\d+)/);
  expect(match, `unparseable Quit action point: ${message}`).not.toBeNull();
  return point(match[1], match[2]);
}

test('dummy stream runs, plots, pauses, resumes, stops, and shuts down cleanly', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(error.stack || String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  expect(await page.evaluate(() => ({
    stream: typeof window.pjWasmReportStreamingStateProbe,
    quit: typeof window.pjWasmReportQuitActionProbe,
  }))).toEqual({ stream: 'function', quit: 'function' });

  // The built-in streams are the Dummy Streamer, selected first, and the PlotJuggler Bridge.
  let state = await requestStreamingState(page, consoleMessages);
  expect(state).toMatchObject({
    sources: 2,
    source: 'Dummy Streamer',
    active: false,
    datasets: 0,
    catalog: 0,
    paused: false,
  });
  await clickQt(page, screen, state.controls.tab);
  state = await requestStreamingState(page, consoleMessages);
  expect(state.controls.start.x).toBeGreaterThanOrEqual(0);

  const startsBefore = consoleMessages.filter(message => message.includes('Dummy streamer started')).length;
  state = await startStreamWithPhysicalInput(page, screen, consoleMessages, state);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('Dummy streamer started')).length,
    { timeout: 15000 },
  ).toBeGreaterThan(startsBefore);
  state = await waitForStreamingState(
    page,
    consoleMessages,
    value => value.active && value.datasets === 1 && value.catalog === 11 && value.range.max > value.range.min,
  );
  expect(state).toMatchObject({ sources: 2, source: 'Dummy Streamer', active: true, datasets: 1, catalog: 11 });
  expect(state.tree.datasetName).toBe('[stream] Dummy Streamer');
  expect(state.tree.topicName).toBe('dummy/sin_cos');
  expect(state.tree.fieldName).toBe('sin');

  await clickQt(page, screen, state.tree.dataset, 2);
  state = await waitForStreamingState(page, consoleMessages, value => value.tree.topic.x >= 0);
  await clickQt(page, screen, state.tree.topic, 2);
  state = await waitForStreamingState(page, consoleMessages, value => value.tree.field.x >= 0);
  await dragQtCanvas(
    page,
    { x: screen.x + state.tree.field.x, y: screen.y + state.tree.field.y },
    { x: screen.x + state.controls.canvas.x, y: screen.y + state.controls.canvas.y },
  );

  state = await waitForStreamingState(
    page,
    consoleMessages,
    value => value.curve.plotted && value.curve.samples >= 20 && value.curve.vertices > 0,
  );
  expect(state.curve.title).toContain('sin');
  expect(state.curve.last.x).toBeGreaterThan(state.curve.first.x);
  expect(Math.abs(state.curve.first.y)).toBeLessThanOrEqual(1.001);
  expect(Math.abs(state.curve.last.y)).toBeLessThanOrEqual(1.001);
  await assertPlotReadback(page, consoleMessages);

  // Let the five-second retention window turn over. Sample/range bounds prove
  // the browser is not accumulating an unbounded stream tail.
  state = await waitForStreamingState(
    page,
    consoleMessages,
    value => (value.range.max - value.range.min) >= 4.5,
    30000,
  );
  expect(state.range.max - state.range.min).toBeLessThanOrEqual(6.5);
  expect(state.curve.samples).toBeLessThanOrEqual(750);
  expect(state.current).toBeCloseTo(state.range.max, 3);

  await clickQt(page, screen, state.controls.pause);
  state = await waitForStreamingState(page, consoleMessages, value => value.paused);
  await page.waitForTimeout(300);
  const pausedStart = await requestStreamingState(page, consoleMessages);
  await page.waitForTimeout(800);
  const pausedEnd = await requestStreamingState(page, consoleMessages);
  expect(pausedEnd.curve.samples).toBe(pausedStart.curve.samples);
  expect(pausedEnd.range).toEqual(pausedStart.range);
  expect(pausedEnd.current).toBe(pausedStart.current);

  await clickQt(page, screen, pausedEnd.controls.pause);
  state = await waitForStreamingState(
    page,
    consoleMessages,
    value => !value.paused && value.curve.last.x > pausedEnd.curve.last.x && value.range.max > pausedEnd.range.max,
  );
  await assertPlotReadback(page, consoleMessages);

  const stopsBefore = consoleMessages.filter(message => message.includes('Dummy streamer stopped')).length;
  await clickQt(page, screen, state.controls.menu);
  state = await waitForStreamingState(page, consoleMessages, value => value.controls.remove.x >= 0);
  await clickQt(page, screen, state.controls.remove);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('Dummy streamer stopped')).length,
    { timeout: 15000 },
  ).toBeGreaterThan(stopsBefore);
  state = await waitForStreamingState(
    page,
    consoleMessages,
    value => !value.active && value.datasets === 0 && value.catalog === 0 && !value.curve.plotted,
  );
  expect(state.sources).toBe(2);

  // Start once more, then close through the real File -> Quit action. The
  // WASM close boundary must cooperatively stop and join the worker even
  // when Emscripten keeps the runtime alive; the stop diagnostic is our witness.
  const secondStart = consoleMessages.filter(message => message.includes('Dummy streamer started')).length;
  state = await startStreamWithPhysicalInput(page, screen, consoleMessages, state);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('Dummy streamer started')).length,
    { timeout: 15000 },
  ).toBeGreaterThan(secondStart);
  state = await waitForStreamingState(
    page,
    consoleMessages,
    value => value.active && value.datasets === 1 && value.catalog === 11,
  );

  const fileMenu = await fileMenuPoint(page, consoleMessages);
  await clickQt(page, screen, fileMenu);
  const quitAction = await quitActionPoint(page, consoleMessages);
  const shutdownStops = consoleMessages.filter(message => message.includes('Dummy streamer stopped')).length;
  await clickQt(page, screen, quitAction);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('Dummy streamer stopped')).length,
    { timeout: 15000 },
  ).toBeGreaterThan(shutdownStops);

  expect(errors).toEqual([]);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_STREAM_STATE_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_MENU_ACTION_FAILED quit'))).toBe(false);
});

// Each plot's drawn curve titles, or null when the probe's summary line never arrived (Firefox can
// drop a line the main thread prints while a stopping stream thread logs), so a poll asks again.
async function plotTitles(page, messages) {
  try {
    const state = await requestMultiPlotState(page, messages, count(messages, 'PJ_WASM_MULTI_SUMMARY'));
    return state.plots.filter(plot => plot.vertices > 0).map(plot => plot.titles);
  } catch (error) {
    return null;
  }
}

test('a stream preset starts its saved streams with no dialog and binds their curves as topics arrive', async ({ page }) => {
  test.setTimeout(180000);
  const { messages, errors, screen } = await bootApp(page);

  // The deployment origin is filled in from this page; a re-save keeps the template.
  const template = '{"url":"${deployment.ws_origin}/bridge"}';
  const sawtooth = [{ id: 'plot_c', curves: [['dummy/sawtooth', 'value', 'c']] }];
  const server = await startPresetServer({
    'live/layout.pj4.xml': streamPreset(
      [{ id: 'a', config: template }, { id: 'b', config: '{}' }],
      [{ id: 'plot_a', curves: [['dummy/sin_cos', 'sin', 'a']] }, { id: 'plot_b', curves: [['dummy/sin_cos', 'cos', 'b']] }]),
    'missing/layout.pj4.xml': streamPreset(
      [{ id: 'c', config: '{}', plugin: '<plugin ID="Missing Stream" manifest_id="missing-stream">' }], sawtooth),
    'typo/layout.pj4.xml': streamPreset([{ id: 'c', config: '{"url":"${deployment.wsorigin}"}' }], sawtooth),
    'next/layout.pj4.xml': streamPreset([{ id: 'c', config: '{}' }], sawtooth),
  });
  try {
    const started = count(messages, 'Dummy streamer started');
    expect(await openPreset(page, messages, server.url('live/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    expect(count(messages, 'PJ_WASM_SOURCE_LAYOUT_DECISION')).toBe(0);
    await expect.poll(() => count(messages, 'Dummy streamer started'), { timeout: 15000 }).toBe(started + 2);
    // Each curve waits for its own stream's topic, then draws it.
    await expect.poll(() => plotTitles(page, messages), { timeout: 30000 })
      .toEqual([['dummy/sin_cos/sin'], ['dummy/sin_cos/cos']]);
    expect(await datasetCount(page, messages)).toBe(2);
    await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
    await expect.poll(() => latest(messages, 'PJ_WASM_LAYOUT_DIALOG present=')).toContain('present=0');

    // Save Layout with Sources writes each stream's record, its template unexpanded;
    // a generic save carries no stream at all.
    const saved = (await downloadSourceLayoutFromFileMenu(page, screen, messages)).bytes.toString('utf8');
    expect(saved).toContain('<previouslyLoaded_StreamingSources>');
    expect(saved).toContain(template);
    expect(saved).not.toContain('ws://127.0.0.1');
    expect([...saved.matchAll(/<streamInfo\b[^>]*\bid="([^"]*)"/g)].map(match => match[1])).toEqual(['a', 'b']);
    expect([...saved.matchAll(/<curve\b[^>]*\bdataset_path="([^"]*)"/g)].map(match => match[1]).sort())
      .toEqual(['pj-stream://a', 'pj-stream://b']);
    const generic = (await downloadLayoutFromFileMenu(page, screen, messages)).bytes.toString('utf8');
    expect(generic).not.toContain('streamInfo');
    expect(generic).not.toContain('pj-stream://');

    // A preset whose stream cannot start fails with its reason and leaves both running.
    // Stops are counted on the main thread's line, printed before the stream thread exits:
    // Firefox can drop the plugin's own "stopped", which that exiting thread prints.
    const STOPPING = 'pj.app.streamingmanager: stopping stream';
    const stopped = count(messages, STOPPING);
    for (const [folder, reason] of [['missing', 'stream'], ['typo', 'stream-config']]) {
      const outcome = await openPreset(page, messages, server.url(`${folder}/layout.pj4.xml`));
      expect(outcome, folder).toContain('PJ_WASM_LAYOUT_LOAD_FAILED');
      expect(outcome, folder).toContain(`reason=${reason}`);
      await dismissDialogs(page, messages);
      expect(await datasetCount(page, messages), folder).toBe(2);
      expect(count(messages, STOPPING), folder).toBe(stopped);
    }
    await expect.poll(() => plotTitles(page, messages), { timeout: 30000 })
      .toEqual([['dummy/sin_cos/sin'], ['dummy/sin_cos/cos']]);

    // The next preset that loads replaces this one's streams.
    expect(await openPreset(page, messages, server.url('next/layout.pj4.xml'))).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expect.poll(() => count(messages, STOPPING)).toBe(stopped + 2);
    await expect.poll(() => plotTitles(page, messages), { timeout: 30000 }).toEqual([['dummy/sawtooth/value']]);
    expect(await datasetCount(page, messages)).toBe(1);
    expect(errors).toEqual([]);
  } finally {
    await server.close();
  }
});
