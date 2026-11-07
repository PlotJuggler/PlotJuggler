// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const { observeRuntimeTarget, replaceRuntimeTarget, waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { clickActiveDialogButton } = require('./support/dialogs');
const { elementAttribute } = require('./support/layout_xml');
const { openFileChooser, openLayoutChooser, openSourceReplayChooser } = require('./support/pickers');
const { requestPlotState } = require('./support/plot_probes');
const { sourceLayoutDecisionGeometry, downloadSourceLayoutFromFileMenu } = require('./support/source_layout');

async function requestFilterEditorState(page, consoleMessages) {
  const previousEditors = consoleMessages.filter(
    message => message.includes('PJ_WASM_FILTER_EDITOR absolute='),
  ).length;
  const previousPreviews = consoleMessages.filter(
    message => message.includes('PJ_WASM_FILTER_PREVIEW title_b64='),
  ).length;
  await page.evaluate(() => window.pjWasmReportFilterEditorProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_FILTER_EDITOR absolute=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousEditors);
  const message = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_FILTER_EDITOR absolute='),
  ).at(-1) || '';
  const match = message.match(
    /absolute=(-?\d+),(-?\d+) apply=(-?\d+),(-?\d+) selected=([^ ]*) apply_enabled=(\d+) preview_curves=(\d+)/,
  );
  expect(match, `unparseable Filter Editor state: ${message}`).not.toBeNull();
  const previewCurveCount = Number(match[7]);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_FILTER_PREVIEW title_b64=')).length,
    { timeout: 10000 },
  ).toBeGreaterThanOrEqual(previousPreviews + previewCurveCount);
  const previews = consoleMessages
    .filter(entry => entry.includes('PJ_WASM_FILTER_PREVIEW title_b64='))
    .slice(previousPreviews)
    .map((entry) => {
      const preview = entry.match(
        /title_b64=([A-Za-z0-9+/=]+) visible=(\d+) samples=(\d+) y=([^ ]*) truncated=(\d+)/,
      );
      expect(preview, `unparseable Filter Editor preview: ${entry}`).not.toBeNull();
      return {
        title: Buffer.from(preview[1], 'base64').toString('utf8'),
        visible: preview[2] === '1',
        samples: Number(preview[3]),
        y: preview[4] ? preview[4].split(',').map(Number) : [],
        truncated: preview[5] === '1',
      };
    });
  return {
    absolute: { x: Number(match[1]), y: Number(match[2]) },
    apply: { x: Number(match[3]), y: Number(match[4]) },
    selected: match[5],
    applyEnabled: match[6] === '1',
    previewCurveCount,
    previews,
  };
}

async function requestFilterResult(page, consoleMessages) {
  const previousResults = consoleMessages.filter(
    message => message.includes('PJ_WASM_FILTER_RESULT visible_plots='),
  ).length;
  const previousCurves = consoleMessages.filter(
    message => message.includes('PJ_WASM_FILTER_CURVE title_b64='),
  ).length;
  await page.evaluate(() => window.pjWasmReportFilterResultProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_FILTER_RESULT visible_plots=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousResults);
  const recipesMessage = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_FILTER_RECIPES count='),
  ).at(-1) || '';
  const recipes = recipesMessage.match(/count=(\d+) ids=([^ ]*) outputs_b64=([^ ]*)/);
  expect(recipes, `unparseable filter recipes: ${recipesMessage}`).not.toBeNull();
  const resultMessage = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_FILTER_RESULT visible_plots='),
  ).at(-1) || '';
  const result = resultMessage.match(/visible_plots=(\d+) visible_curves=(\d+)/);
  expect(result, `unparseable filter result: ${resultMessage}`).not.toBeNull();
  const curves = consoleMessages
    .filter(entry => entry.includes('PJ_WASM_FILTER_CURVE title_b64='))
    .slice(previousCurves)
    .map((entry) => {
      const curve = entry.match(
        /title_b64=([A-Za-z0-9+/=]+) source_b64=([A-Za-z0-9+/=]+) samples=(\d+) y=([^ ]*) truncated=(\d+)/,
      );
      expect(curve, `unparseable filtered curve: ${entry}`).not.toBeNull();
      return {
        title: Buffer.from(curve[1], 'base64').toString('utf8'),
        source: Buffer.from(curve[2], 'base64').toString('utf8'),
        samples: Number(curve[3]),
        y: curve[4] ? curve[4].split(',').map(Number) : [],
        truncated: curve[5] === '1',
      };
    });
  return {
    recipeCount: Number(recipes[1]),
    processorIds: recipes[2] ? recipes[2].split(',') : [],
    outputNames: recipes[3]
      ? recipes[3].split(',').map(encoded => Buffer.from(encoded, 'base64').toString('utf8'))
      : [],
    visiblePlots: Number(result[1]),
    visibleCurves: Number(result[2]),
    curves,
  };
}

async function requestTransformEditorState(page, consoleMessages) {
  const previousEditors = consoleMessages.filter(
    message => message.includes('PJ_WASM_TRANSFORM_EDITOR open='),
  ).length;
  const previousPreviews = consoleMessages.filter(
    message => message.includes('PJ_WASM_TRANSFORM_PREVIEW title_b64='),
  ).length;
  const previousDetails = consoleMessages.filter(
    message => message.includes('PJ_WASM_TRANSFORM_EDITOR_DETAIL name_b64='),
  ).length;
  await page.evaluate(() => window.pjWasmReportTransformEditorProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_TRANSFORM_EDITOR open=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousEditors);
  const message = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_TRANSFORM_EDITOR open='),
  ).at(-1) || '';
  const match = message.match(
    /open=(\d+) add=(-?\d+),(-?\d+) source=(-?\d+),(-?\d+) drop=(-?\d+),(-?\d+) name=(-?\d+),(-?\d+) function=(-?\d+),(-?\d+) create=(-?\d+),(-?\d+) create_enabled=(\d+) source_rows=(\d+) preview_curves=(\d+)/,
  );
  expect(match, `unparseable Transform Editor state: ${message}`).not.toBeNull();
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_TRANSFORM_EDITOR_DETAIL name_b64=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousDetails);
  const detailMessage = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_TRANSFORM_EDITOR_DETAIL name_b64='),
  ).at(-1) || '';
  const detail = detailMessage.match(
    /name_b64=([A-Za-z0-9+/=]*) function_b64=([A-Za-z0-9+/=]*) terminal_visible=(\d+) terminal_b64=([A-Za-z0-9+/=]*) ticks=(\d+) events=(\d+) applies=(\d+) timer_active=(\d+) dialogs_b64=([A-Za-z0-9+/=]*)/,
  );
  expect(detail, `unparseable Transform Editor detail: ${detailMessage}`).not.toBeNull();
  const previewCurveCount = Number(match[16]);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_TRANSFORM_PREVIEW title_b64=')).length,
    { timeout: 10000 },
  ).toBeGreaterThanOrEqual(previousPreviews + previewCurveCount);
  const previews = consoleMessages
    .filter(entry => entry.includes('PJ_WASM_TRANSFORM_PREVIEW title_b64='))
    .slice(previousPreviews)
    .map((entry) => {
      const preview = entry.match(
        /title_b64=([A-Za-z0-9+/=]+) samples=(\d+) first=([^ ]+) last=([^ ]+) dashed=(\d+)/,
      );
      expect(preview, `unparseable Transform Editor preview: ${entry}`).not.toBeNull();
      return {
        title: Buffer.from(preview[1], 'base64').toString('utf8'),
        samples: Number(preview[2]),
        first: Number(preview[3]),
        last: Number(preview[4]),
        dashed: preview[5] === '1',
      };
    });
  return {
    open: match[1] === '1',
    add: { x: Number(match[2]), y: Number(match[3]) },
    source: { x: Number(match[4]), y: Number(match[5]) },
    drop: { x: Number(match[6]), y: Number(match[7]) },
    name: { x: Number(match[8]), y: Number(match[9]) },
    function: { x: Number(match[10]), y: Number(match[11]) },
    create: { x: Number(match[12]), y: Number(match[13]) },
    createEnabled: match[14] === '1',
    sourceRows: Number(match[15]),
    previewCurveCount,
    previews,
    nameValue: Buffer.from(detail[1], 'base64').toString('utf8'),
    functionValue: Buffer.from(detail[2], 'base64').toString('utf8'),
    terminalVisible: detail[3] === '1',
    terminalValue: Buffer.from(detail[4], 'base64').toString('utf8'),
    ticks: Number(detail[5]),
    events: Number(detail[6]),
    applies: Number(detail[7]),
    timerActive: detail[8] === '1',
    visibleDialogs: Buffer.from(detail[9], 'base64').toString('utf8'),
  };
}

async function requestToolboxTransformResult(page, consoleMessages) {
  const previousSummaries = consoleMessages.filter(
    message => message.includes('PJ_WASM_TOOLBOX_TRANSFORMS count='),
  ).length;
  const previousRecipes = consoleMessages.filter(
    message => message.includes('PJ_WASM_TOOLBOX_TRANSFORM key_b64='),
  ).length;
  await page.evaluate(() => window.pjWasmReportToolboxTransformResultProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_TOOLBOX_TRANSFORMS count=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousSummaries);
  const summaryMessage = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_TOOLBOX_TRANSFORMS count='),
  ).at(-1) || '';
  const summary = summaryMessage.match(/count=(\d+) materializing=(\d)/);
  expect(summary, `unparseable toolbox transform summary: ${summaryMessage}`).not.toBeNull();
  const count = Number(summary[1]);
  const materializing = summary[2] === '1';
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_TOOLBOX_TRANSFORM key_b64=')).length,
    { timeout: 10000 },
  ).toBeGreaterThanOrEqual(previousRecipes + count);
  const recipes = consoleMessages
    .filter(entry => entry.includes('PJ_WASM_TOOLBOX_TRANSFORM key_b64='))
    .slice(previousRecipes)
    .map((entry) => {
      const recipe = entry.match(
        /key_b64=([A-Za-z0-9+/=]+) owner_b64=([A-Za-z0-9+/=]+) user_b64=([A-Za-z0-9+/=]+) backend=([^ ]+) inputs_b64=([^ ]*) outputs_b64=([^ ]*) ephemeral=(\d+) custom=(-?\d+),(-?\d+)/,
      );
      expect(recipe, `unparseable toolbox transform: ${entry}`).not.toBeNull();
      const decodeList = value => value
        ? value.split(',').map(encoded => Buffer.from(encoded, 'base64').toString('utf8'))
        : [];
      return {
        key: Buffer.from(recipe[1], 'base64').toString('utf8'),
        owner: Buffer.from(recipe[2], 'base64').toString('utf8'),
        userId: Buffer.from(recipe[3], 'base64').toString('utf8'),
        backend: recipe[4],
        inputs: decodeList(recipe[5]),
        outputs: decodeList(recipe[6]),
        ephemeral: recipe[7] === '1',
        custom: { x: Number(recipe[8]), y: Number(recipe[9]) },
      };
    });
  return { count, materializing, recipes };
}

test('Luau Filter Editor applies and restores an exact source-bound processor through real browser UI', async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const allConsoleMessages = [];
  let consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  observeRuntimeTarget(page, consoleMessages, allConsoleMessages, errors);

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  let screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  const sourceBytes = Buffer.from('time,temp,value\n0,-2,1\n1,4,3\n2,-6,2\n');

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'filter-values.csv',
    mimeType: 'text/csv',
    buffer: sourceBytes,
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('scalar_series=filter-values/temp,filter-values/time,filter-values/value');

  // Create the origin curve through CurveTreeView's real QDrag MIME path.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  const placeholder = { x: screen.x + 820, y: screen.y + 390 };
  await dragQtCanvas(page, { x: screen.x + 95, y: screen.y + 232 }, placeholder);
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PLOT_RHI_READY')),
    { timeout: 15000 },
  ).toBe(true);
  let plotStateMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_PLOT_STATE count='),
  ).length;
  let plotState = await requestPlotState(page, consoleMessages, plotStateMessages);
  plotStateMessages += 1;
  expect(plotState.titles).toEqual(['filter-values/temp']);

  // Open the production plot context menu, observe only the QAction geometry,
  // and activate it with a physical mouse click.
  const canvasCenter = {
    x: screen.x + plotState.canvas.x + (plotState.canvas.width / 2),
    y: screen.y + plotState.canvas.y + (plotState.canvas.height / 2),
  };
  await page.mouse.click(canvasCenter.x, canvasCenter.y, { button: 'right' });
  await page.waitForTimeout(250);
  const actionMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=apply_filter'),
  ).length;
  await page.evaluate(() => window.pjWasmReportApplyFilterActionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=apply_filter')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(actionMessages);
  const actionMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=apply_filter'),
  ).at(-1) || '';
  const actionCenter = actionMessage.match(/center=(\d+),(\d+)/);
  expect(actionCenter, `unparseable Apply Filter geometry: ${actionMessage}`).not.toBeNull();
  await page.mouse.click(screen.x + Number(actionCenter[1]), screen.y + Number(actionCenter[2]));

  let editor = await requestFilterEditorState(page, consoleMessages);
  expect(editor.selected).toBe('none');
  expect(editor.applyEnabled).toBe(false);
  expect(editor.absolute.x).toBeGreaterThanOrEqual(0);
  expect(editor.absolute.y).toBeGreaterThanOrEqual(0);

  // Pick the parameterless bundled Luau Absolute class with real list input.
  await page.mouse.click(screen.x + editor.absolute.x, screen.y + editor.absolute.y);
  await page.waitForTimeout(500);
  editor = await requestFilterEditorState(page, consoleMessages);
  expect(editor.selected).toBe('absolute');
  expect(editor.applyEnabled).toBe(true);
  const filteredPreview = editor.previews.find(preview => preview.title === 'filter-values/temp[Absolute]');
  expect(filteredPreview).toBeDefined();
  expect(filteredPreview.visible).toBe(true);
  expect(filteredPreview.samples).toBe(3);
  expect(filteredPreview.y).toEqual([2, 4, 6]);
  expect(filteredPreview.truncated).toBe(false);

  // Apply through the production button and verify the live materialized curve,
  // recipe identity, and exact transformed samples.
  await page.mouse.click(screen.x + editor.apply.x, screen.y + editor.apply.y);
  const applied = await requestFilterResult(page, consoleMessages);
  expect(applied.recipeCount).toBe(1);
  expect(applied.processorIds).toEqual(['absolute']);
  expect(applied.outputNames).toEqual(['filter-values/temp[Absolute]']);
  expect(applied.visiblePlots).toBe(1);
  expect(applied.visibleCurves).toBe(1);
  expect(applied.curves).toHaveLength(1);
  expect(applied.curves[0].title).toBe('filter-values/temp[Absolute]/value');
  expect(applied.curves[0].samples).toBe(3);
  expect(applied.curves[0].y).toEqual([2, 4, 6]);
  expect(applied.curves[0].truncated).toBe(false);
  plotState = await requestPlotState(page, consoleMessages, plotStateMessages);
  plotStateMessages += 1;
  expect(plotState.titles).toEqual(['filter-values/temp[Absolute]/value']);

  const downloaded = await downloadSourceLayoutFromFileMenu(page, screen, consoleMessages);
  expect(downloaded.fileName).toBe('plotjuggler-source-layout.pj4.xml');
  expect(elementAttribute(downloaded.bytes, 'processor', 'processor_id')).toBe('absolute');
  expect(elementAttribute(downloaded.bytes, 'processor', 'input_topic')).toBe('filter-values');
  expect(elementAttribute(downloaded.bytes, 'processor', 'input_field')).toBe('temp');
  expect(elementAttribute(downloaded.bytes, 'processor', 'output_name')).toBe('filter-values/temp[Absolute]');
  const xml = downloaded.bytes.toString('utf8');
  expect(xml).toContain('<source_fallback>');
  expect(xml).not.toContain('pj-upload://');
  expect(xml).not.toContain('/pj_uploads');

  // Recreate the runtime in a fresh Page target, retaining the same browser
  // context but no session, Luau VM, staged file, upload token, or chooser
  // interception state. The fallback-picker condition is installed again
  // before navigation.
  ({ page, consoleMessages, screen } = await replaceRuntimeTarget(
    page,
    allConsoleMessages,
    errors,
    { hideOpenFilePicker: true },
  ));

  const decisionCount = consoleMessages.filter(
    message => message.includes('PJ_WASM_SOURCE_LAYOUT_DECISION'),
  ).length;
  const layoutChooser = await openLayoutChooser(page);
  await layoutChooser.setFiles({
    name: downloaded.fileName,
    mimeType: 'application/xml',
    buffer: downloaded.bytes,
  });
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_SOURCE_LAYOUT_DECISION')).length,
    { timeout: 15000 },
  ).toBeGreaterThan(decisionCount);
  const decision = await sourceLayoutDecisionGeometry(page, consoleMessages);
  const replayChooser = await openSourceReplayChooser(page, screen, decision, consoleMessages);
  await replayChooser.setFiles({
    name: 'filter-values.csv',
    mimeType: 'text/csv',
    buffer: sourceBytes,
  });
  await expect.poll(
    () => consoleMessages.find(
      message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=plotjuggler-source-layout'),
    ) || '',
    { timeout: 30000 },
  ).toContain('plotjuggler-source-layout.pj4.xml');

  const restored = await requestFilterResult(page, consoleMessages);
  expect(restored.recipeCount).toBe(1);
  expect(restored.processorIds).toEqual(['absolute']);
  expect(restored.outputNames).toEqual(['filter-values/temp[Absolute]']);
  expect(restored.visiblePlots).toBe(1);
  expect(restored.visibleCurves).toBe(1);
  expect(restored.curves).toHaveLength(1);
  expect(restored.curves[0].title).toBe('filter-values/temp[Absolute]/value');
  expect(restored.curves[0].samples).toBe(3);
  expect(restored.curves[0].y).toEqual([2, 4, 6]);
  expect(restored.curves[0].truncated).toBe(false);
  plotStateMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_PLOT_STATE count='),
  ).length;
  const restoredPlot = await requestPlotState(page, consoleMessages, plotStateMessages);
  expect(restoredPlot.titles).toEqual(['filter-values/temp[Absolute]/value']);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
    { timeout: 10000 },
  ).toMatch(/filter-values\/temp\[Absolute\]\/value:#[0-9a-f]{8}:[1-9]\d*/i);
  expect(allConsoleMessages.some(message => message.includes('PJ_WASM_FILTER_EDITOR_FAILED'))).toBe(false);
  expect(allConsoleMessages.some(message => message.includes('PJ_WASM_FILTER_RESULT_FAILED'))).toBe(false);
  expect(allConsoleMessages.some(message => message.includes('PJ_WASM_MENU_ACTION_FAILED apply_filter'))).toBe(false);
  expect(errors).toEqual([]);
});

// @serial: its heartbeat bound fails on a loaded runner (playwright.config.js).
test('Transform Editor creates a large exact Luau series while the browser main thread stays responsive', {
  tag: '@serial',
}, async ({ page }) => {
  test.setTimeout(240000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  // Large enough to make both the preview and persistent eager Luau nodes do
  // material work, while retaining a deterministic output for every row.
  const sampleCount = 30000;
  const rows = ['time,temp'];
  for (let index = 0; index < sampleCount; ++index) {
    rows.push(`${index / 1000},${(index % 7) - 3}`);
  }
  const sourceBytes = Buffer.from(`${rows.join('\n')}\n`);
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'toolbox-values.csv',
    mimeType: 'text/csv',
    buffer: sourceBytes,
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 60000 },
  ).toContain('scalar_series=toolbox-values/temp,toolbox-values/time');

  // Expand the production CurveTreeView, then use the observation probe only
  // to locate the real source row and Custom Series add button.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await page.mouse.click(screen.x + 30, screen.y + 212);
  let editor = await requestTransformEditorState(page, consoleMessages);
  expect(editor.open).toBe(false);
  expect(editor.add.x).toBeGreaterThanOrEqual(0);
  expect(editor.add.y).toBeGreaterThanOrEqual(0);
  expect(editor.source.x).toBeGreaterThanOrEqual(0);
  expect(editor.source.y).toBeGreaterThanOrEqual(0);

  await page.mouse.click(screen.x + editor.add.x, screen.y + editor.add.y);
  await expect.poll(async () => {
    editor = await requestTransformEditorState(page, consoleMessages);
    return editor.open;
  }, { timeout: 15000 }).toBe(true);
  expect(editor.drop.x).toBeGreaterThanOrEqual(0);
  expect(editor.drop.y).toBeGreaterThanOrEqual(0);

  // Drop the real catalog item through CurveTreeView's QDrag MIME path.
  await dragQtCanvas(
    page,
    { x: screen.x + editor.source.x, y: screen.y + editor.source.y },
    { x: screen.x + editor.drop.x, y: screen.y + editor.drop.y },
  );
  await expect.poll(async () => {
    editor = await requestTransformEditorState(page, consoleMessages);
    return editor.sourceRows;
  }, { timeout: 60000 }).toBe(1);

  // Program the transform through the actual Qt text editor; its name is asked by Create.
  await page.mouse.click(screen.x + editor.function.x, screen.y + editor.function.y);
  await page.keyboard.press('Control+A');
  await page.keyboard.type('return value * value + 3');

  // Qt runs on the page's main thread (the build has no PROXY_TO_PTHREAD), so
  // each report call runs synchronously inside Qt's event loop. Give the
  // production 20 Hz panel timer an observation-free window to rebuild the
  // 30k-row preview; repeatedly queueing reports while that loop is busy can
  // itself starve the timer this observation is waiting for.
  await page.waitForTimeout(10000);
  const observedEditorStates = [];
  for (let attempt = 0; attempt < 6; ++attempt) {
    if (attempt > 0) {
      await page.waitForTimeout(10000);
    }
    editor = await requestTransformEditorState(page, consoleMessages);
    observedEditorStates.push({
      createEnabled: editor.createEnabled,
      previewCurveCount: editor.previewCurveCount,
      nameValue: editor.nameValue,
      functionValue: editor.functionValue,
      terminalVisible: editor.terminalVisible,
      terminalValue: editor.terminalValue,
      ticks: editor.ticks,
      events: editor.events,
      applies: editor.applies,
      timerActive: editor.timerActive,
      visibleDialogs: editor.visibleDialogs,
    });
    if (editor.createEnabled && editor.previewCurveCount === 2) {
      break;
    }
  }
  expect(
    editor.createEnabled && editor.previewCurveCount === 2,
    `Transform Editor never became ready: ${JSON.stringify(observedEditorStates)}`,
  ).toBe(true);
  const ghost = editor.previews.find(preview => preview.title === 'toolbox-values/temp');
  // The transformed preview is named after the output the trial inferred, not yet the recipe.
  const transformed = editor.previews.find(preview => preview.title !== 'toolbox-values/temp');
  // The input "ghost" and the transformed preview are told apart by colour,
  // not dash style, so only their data is asserted here.
  expect(ghost).toBeDefined();
  expect(ghost.samples).toBe(2000);
  expect(transformed).toBeDefined();
  expect(transformed.samples).toBe(2000);
  expect(transformed.first).toBe(12);

  // Create asks for the name in the editor's prompt; its OK installs the recipe.
  await page.mouse.click(screen.x + editor.create.x, screen.y + editor.create.y);
  await expect.poll(async () => {
    editor = await requestTransformEditorState(page, consoleMessages);
    return editor.name.x >= 0 && editor.name.y >= 0;
  }, { timeout: 15000 }).toBe(true);
  await page.mouse.click(screen.x + editor.name.x, screen.y + editor.name.y);
  await page.keyboard.press('Control+A');
  await page.keyboard.type('toolbox-square');

  // The Qt app and its Luau replay share the page's main thread. A page
  // heartbeat must keep ticking from the physical OK click until the replay
  // has materialized every row: no single slice of that work may hold the
  // thread for 100 ms. Production announces the transform after the first
  // slice; the series then grows until complete.
  await page.evaluate(() => {
    const heartbeat = { started: performance.now(), ticks: [], timer: 0 };
    heartbeat.timer = setInterval(() => heartbeat.ticks.push(performance.now()), 4);
    globalThis.__pjWasmTransformHeartbeat = heartbeat;
  });
  const readyMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_TOOLBOX_TRANSFORM_READY plugin=toolbox-transform-editor'),
  ).length;
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_TOOLBOX_TRANSFORM_READY plugin=toolbox-transform-editor'),
    ).length,
    { timeout: 60000 },
  ).toBeGreaterThan(readyMessages);
  await expect.poll(
    async () => (await requestToolboxTransformResult(page, consoleMessages)).materializing,
    { timeout: 60000 },
  ).toBe(false);
  const heartbeat = await page.evaluate(() => {
    const state = globalThis.__pjWasmTransformHeartbeat;
    clearInterval(state.timer);
    return { started: state.started, finished: performance.now(), ticks: state.ticks };
  });
  const heartbeatPoints = [heartbeat.started, ...heartbeat.ticks, heartbeat.finished];
  const heartbeatGaps = heartbeatPoints.slice(1).map((value, index) => value - heartbeatPoints[index]);
  expect(heartbeat.finished - heartbeat.started).toBeGreaterThan(75);
  expect(heartbeat.ticks.length).toBeGreaterThan(2);
  expect(Math.max(...heartbeatGaps)).toBeLessThan(100);

  const toolboxResult = await requestToolboxTransformResult(page, consoleMessages);
  expect(toolboxResult.count).toBe(1);
  expect(toolboxResult.recipes).toHaveLength(1);
  expect(toolboxResult.recipes[0]).toMatchObject({
    key: 'toolbox-transform-editor/toolbox-square',
    owner: 'toolbox-transform-editor',
    userId: 'toolbox-square',
    backend: 'luau',
    inputs: ['toolbox-values/temp'],
    outputs: ['toolbox-square'],
    ephemeral: false,
  });
  expect(toolboxResult.recipes[0].custom.x).toBeGreaterThanOrEqual(0);
  expect(toolboxResult.recipes[0].custom.y).toBeGreaterThanOrEqual(0);

  // Drag the plugin-created Custom Series row into the real plot and verify all
  // 30k materialized samples, including an exact deterministic prefix.
  const placeholder = { x: screen.x + 820, y: screen.y + 390 };
  const rhiReadyMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_PLOT_RHI_READY'),
  ).length;
  await dragQtCanvas(
    page,
    {
      x: screen.x + toolboxResult.recipes[0].custom.x,
      y: screen.y + toolboxResult.recipes[0].custom.y,
    },
    placeholder,
  );
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_RHI_READY')).length,
    { timeout: 15000 },
  ).toBeGreaterThan(rhiReadyMessages);
  let plotted;
  await expect.poll(async () => {
    plotted = await requestFilterResult(page, consoleMessages);
    return plotted.curves[0]?.samples;
  }, { timeout: 30000 }).toBe(sampleCount);
  expect(plotted.recipeCount).toBe(0);
  expect(plotted.visiblePlots).toBe(1);
  expect(plotted.visibleCurves).toBe(1);
  expect(plotted.curves).toHaveLength(1);
  expect(plotted.curves[0].title).toBe('toolbox-square/value');
  expect(plotted.curves[0].samples).toBe(sampleCount);
  expect(plotted.curves[0].y).toEqual(
    Array.from({ length: 32 }, (_, index) => (((index % 7) - 3) ** 2) + 3),
  );
  expect(plotted.curves[0].truncated).toBe(true);
  const plotState = await requestPlotState(
    page,
    consoleMessages,
    consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length,
  );
  expect(plotState.titles).toEqual(['toolbox-square/value']);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_CURVE_PIXELS')).at(-1) || '',
    { timeout: 10000 },
  ).toMatch(/toolbox-square\/value:#[0-9a-f]{8}:[1-9]\d*/i);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_TRANSFORM_EDITOR_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_TOOLBOX_TRANSFORM_RESULT_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});
