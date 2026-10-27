// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const crypto = require('crypto');
const fs = require('fs');
const { observeRuntimeTarget, replaceRuntimeTarget, waitForQtApp } = require('./support/app');
const { nativePickerType } = require('./support/browser');
const { clickActiveDialogButton } = require('./support/dialogs');
const {
  elementAttribute, elementAttributes, setElementAttribute, setElementAttributeAt,
} = require('./support/layout_xml');
const { downloadLayoutFromFileMenu, saveLayoutActionCenter } = require('./support/layout_actions');
const { openFileChooser, openLayoutChooser, openSourceReplayChooser } = require('./support/pickers');
const { requestPlotState } = require('./support/plot_probes');
const { sourceLayoutDecisionGeometry, downloadSourceLayoutFromFileMenu } = require('./support/source_layout');
const { curveRowCenter, placeholderIconCenter } = require('./support/geometry');

async function pickerApisRemainExact(page) {
  return page.evaluate(() => {
    const before = globalThis.__pjWasmPickerApisBefore;
    return before !== undefined
      && window.showOpenFilePicker === before.open
      && window.showSaveFilePicker === before.save
      && Object.prototype.hasOwnProperty.call(window, 'showOpenFilePicker') === before.openOwn
      && Object.prototype.hasOwnProperty.call(window, 'showSaveFilePicker') === before.saveOwn;
  });
}

function splitterRatio(layoutBytes) {
  const sizes = elementAttribute(layoutBytes, 'chrome_state', 'main_splitter_sizes')
    .split(',')
    .map(Number);
  expect(sizes).toHaveLength(2);
  return sizes[0] / (sizes[0] + sizes[1]);
}

function customizeLayout(layoutBytes, leftSeconds, rightSeconds, style = 'Dots', width = '3.0') {
  const leftNs = Math.round(leftSeconds * 1e9);
  const rightNs = Math.round(rightSeconds * 1e9);
  const firstProfile = leftSeconds < 0.4;
  const styleId = style === 'Dots' ? '1' : '0';
  let xml = layoutBytes.toString('utf8');
  xml = xml.replace(/(<plot\b[^>]*\bline_width=")[^"]+/, `$1${width}`);
  xml = xml.replace(/(<plot\b[^>]*\bstyle=")[^"]+/, `$1${style}`);
  xml = xml.replace(
    /<range\b[^>]*\/>/,
    `<range bottom="-2" top="12" left="${leftSeconds}" right="${rightSeconds}" left_ns="${leftNs}" right_ns="${rightNs}" x_basis="absolute"/>`,
  );
  xml = setElementAttribute(xml, 'right_panel_state', 'width', width);
  xml = setElementAttribute(xml, 'right_panel_state', 'style', styleId);
  xml = setElementAttribute(xml, 'left_panel_state', 'sources_tab', 'stream');
  xml = setElementAttribute(xml, 'left_panel_state', 'streaming_buffer', firstProfile ? '37' : '52');
  xml = setElementAttribute(xml, 'curve_list_state', 'show_topics', firstProfile ? 'false' : 'true');
  xml = setElementAttribute(xml, 'curve_list_state', 'show_values', firstProfile ? 'true' : 'false');
  xml = setElementAttribute(
    xml,
    'curve_list_state',
    'datasets_filter',
    firstProfile ? 'workspace-one-filter' : 'workspace-two-filter',
  );
  xml = setElementAttribute(xml, 'chrome_state', 'main_splitter_sizes', firstProfile ? '360,800' : '420,740');
  xml = setElementAttribute(xml, 'source_timeline', 'zoom', firstProfile ? '0.000001' : '0.000002');
  xml = setElementAttribute(xml, 'source_timeline', 'scroll_left_ns', firstProfile ? '500000000' : '750000000');
  xml = setElementAttribute(xml, 'source_timeline', 'scroll_top_px', '0');
  xml = setElementAttribute(xml, 'source_timeline', 'name_column_width', firstProfile ? '240' : '280');
  xml = setElementAttribute(xml, 'source_timeline', 'snap', firstProfile ? 'false' : 'true');
  return Buffer.from(xml);
}

function makeMissingCurveLayout(layoutBytes) {
  return Buffer.from(layoutBytes.toString('utf8').replace(/(<curve\b[^>]*\btopic=")[^"]+/, '$1missing/topic'));
}

function makeInvalidSceneLayout(layoutBytes) {
  const xml = layoutBytes.toString('utf8').replace(
    /(<DockArea\b[^>]*>)[\s\S]*?(<\/DockArea>)/,
    '$1<scene3d><layer object_type="not_a_type"/></scene3d>$2',
  );
  return Buffer.from(xml);
}

async function requestLayoutPersistence(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportLayoutPersistenceProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_PERSISTENCE')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_PERSISTENCE')).at(-1) || '';
  const match = message.match(/recent=(\d+)/);
  expect(match, `unparseable layout persistence: ${message}`).not.toBeNull();
  return Number(match[1]);
}

async function requestLayoutPersistenceState(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportLayoutPersistenceProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_PERSISTENCE')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_PERSISTENCE')).at(-1) || '';
  const match = message.match(/recent=(\d+) files=(\d+)/);
  expect(match, `unparseable layout persistence state: ${message}`).not.toBeNull();
  return { layouts: Number(match[1]), files: Number(match[2]) };
}

async function requestLayoutSnapshot(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportLayoutSnapshotProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_SNAPSHOT base64=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_SNAPSHOT base64=')).at(-1) || '';
  const match = message.match(/base64=([A-Za-z0-9+/=]+)/);
  expect(match, `unparseable layout snapshot: ${message}`).not.toBeNull();
  return Buffer.from(match[1], 'base64');
}

async function requestDatasetCount(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportDatasetCountProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_DATASET_COUNT count=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_DATASET_COUNT count=')).at(-1) || '';
  const match = message.match(/count=(\d+)/);
  expect(match, `unparseable dataset count: ${message}`).not.toBeNull();
  return Number(match[1]);
}

async function requestBrowserPersistenceState(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportPersistenceProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_PERSISTENCE_STATE')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);
  const message = consoleMessages.filter(message => message.includes('PJ_WASM_PERSISTENCE_STATE')).at(-1) || '';
  const match = message.match(
    /theme=(\S+) theme_present=(\d+) precision=(\d+) precision_present=(\d+) unsafe_path=(\d+) unsafe_plugin=(\d+) unsafe_recent=(\d+) recipes=(\d+) durable_keys=(\d+)/,
  );
  expect(match, `unparseable browser persistence state: ${message}`).not.toBeNull();
  return {
    theme: match[1],
    themePresent: Number(match[2]),
    precision: Number(match[3]),
    precisionPresent: Number(match[4]),
    unsafePath: Number(match[5]),
    unsafePlugin: Number(match[6]),
    unsafeRecent: Number(match[7]),
    recipes: Number(match[8]),
    durableKeys: Number(match[9]),
  };
}

async function clickRecentMenuAction(page, consoleMessages, expectedLabel) {
  const buttonCount = consoleMessages.filter(message => message.includes('PJ_WASM_RECENT_BUTTON center=')).length;
  await page.evaluate(() => window.pjWasmReportRecentButtonProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_RECENT_BUTTON center=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(buttonCount);
  const buttonMessage = consoleMessages.filter(message => message.includes('PJ_WASM_RECENT_BUTTON center=')).at(-1) || '';
  const button = buttonMessage.match(/center=(\d+),(\d+) enabled=(\d+) visible=(\d+)/);
  expect(button, `unparseable recent button: ${buttonMessage}`).not.toBeNull();
  expect({ enabled: Number(button[3]), visible: Number(button[4]) }).toEqual({ enabled: 1, visible: 1 });
  await page.mouse.click(Number(button[1]), Number(button[2]));

  const actionCount = consoleMessages.filter(message => message.includes('PJ_WASM_RECENT_ACTION label=')).length;
  await page.evaluate(() => window.pjWasmReportRecentMenuProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_RECENT_ACTION label=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(actionCount);
  const actionMessage = consoleMessages
    .filter(message => message.includes(`PJ_WASM_RECENT_ACTION label=${expectedLabel} center=`))
    .at(-1) || '';
  const action = actionMessage.match(/center=(\d+),(\d+)/);
  expect(action, `missing recent action ${expectedLabel}: ${actionMessage}`).not.toBeNull();
  await page.mouse.click(Number(action[1]), Number(action[2]));
}

test('browser persistence survives refresh through a bounded allowlist and scrubs legacy leaks', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  expect(await page.evaluate(() => ({
    seed: typeof window.pjWasmSeedPersistenceProbe,
    report: typeof window.pjWasmReportPersistenceProbe,
    clear: typeof window.pjWasmClearPersistenceProbe,
  }))).toEqual({ seed: 'function', report: 'function', clear: 'function' });

  await page.evaluate(() => window.pjWasmSeedPersistenceProbe());
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PERSISTENCE_SEEDED')) || '',
    { timeout: 10000 },
  ).toContain('recipes=5');

  // The probe deliberately simulates two unsafe keys from the pre-W9d default
  // backend. Only the next cold construction exercises migration/scrubbing.
  let readyCount = consoleMessages.filter(message => message.includes('PJ_WASM_APP_READY')).length;
  await page.reload();
  await waitForQtApp(page, consoleMessages, readyCount);

  let stateCount = consoleMessages.filter(message => message.includes('PJ_WASM_PERSISTENCE_STATE')).length;
  const restored = await requestBrowserPersistenceState(page, consoleMessages, stateCount);
  expect(restored).toMatchObject({
    theme: 'dark',
    themePresent: 1,
    precision: 6,
    precisionPresent: 1,
    unsafePath: 0,
    unsafePlugin: 0,
    unsafeRecent: 0,
    recipes: 5,
  });
  const durableStorage = await page.evaluate(() => Object.entries(localStorage).flat().join('\n'));
  expect(durableStorage).not.toContain('pj-upload://');
  expect(durableStorage).not.toContain('/pj_uploads');
  expect(durableStorage).not.toContain('secret-token');
  expect(durableStorage).not.toContain('/home/user/private.csv');
  expect(durableStorage).not.toContain('PluginConfig');
  expect(durableStorage).not.toContain('File/recent');

  await page.evaluate(() => window.pjWasmClearPersistenceProbe());
  await expect.poll(
    () => consoleMessages.some(message => message.includes('PJ_WASM_PERSISTENCE_CLEARED')),
    { timeout: 10000 },
  ).toBe(true);
  readyCount = consoleMessages.filter(message => message.includes('PJ_WASM_APP_READY')).length;
  await page.reload();
  await waitForQtApp(page, consoleMessages, readyCount);
  stateCount = consoleMessages.filter(message => message.includes('PJ_WASM_PERSISTENCE_STATE')).length;
  const cleared = await requestBrowserPersistenceState(page, consoleMessages, stateCount);
  expect(cleared).toMatchObject({
    theme: 'light',
    themePresent: 0,
    precision: 3,
    precisionPresent: 0,
    unsafePath: 0,
    unsafePlugin: 0,
    unsafeRecent: 0,
    recipes: 0,
  });
  expect(errors).toEqual([]);
});

test('browser layouts open transactionally and download with exact round-trip state', async ({ page, browserName }) => {
  test.setTimeout(150000);
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  await expect.poll(() => page.evaluate(() => crossOriginIsolated)).toBe(true);
  const pickerApisBefore = await page.evaluate(() => {
    globalThis.__pjWasmPickerApisBefore = {
      open: window.showOpenFilePicker,
      save: window.showSaveFilePicker,
      openOwn: Object.prototype.hasOwnProperty.call(window, 'showOpenFilePicker'),
      saveOwn: Object.prototype.hasOwnProperty.call(window, 'showSaveFilePicker'),
    };
    return {
      open: typeof window.showOpenFilePicker,
      save: typeof window.showSaveFilePicker,
    };
  });
  const nativePicker = nativePickerType(browserName);
  expect(pickerApisBefore).toEqual({ open: nativePicker, save: nativePicker });
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'layout-open.csv',
    mimeType: 'text/csv',
    buffer: Buffer.from('time,temp\n0,0\n1,10\n2,0\n'),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('layout-open/temp:3@');
  { const icon = await placeholderIconCenter(page, screen, consoleMessages, 'plot');
    await page.mouse.click(icon.x, icon.y); }
  await page.waitForTimeout(1500);
  await page.keyboard.press('F9');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '',
    { timeout: 15000 },
  ).toContain('first_curve=layout-open/temp');

  let stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  const original = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(original.count).toBe(1);
  let snapshotMessages = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_SNAPSHOT base64=')).length;
  const canonicalLayout = await requestLayoutSnapshot(page, consoleMessages, snapshotMessages);
  snapshotMessages += 1;
  const persistenceMessages = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_PERSISTENCE')).length;
  const recentBefore = await requestLayoutPersistence(page, consoleMessages, persistenceMessages);

  // Cancellation is a real empty file selection, not a synthetic callback.
  const cancelledChooser = await openLayoutChooser(page);
  await cancelledChooser.setFiles([]);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_PICKER_CANCELLED')).length,
    { timeout: 10000 },
  ).toBe(1);
  const afterCancel = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(afterCancel.titles).toEqual(original.titles);
  expect(afterCancel.view).toEqual(original.view);

  // A named zero-byte/invalid file must be reported as malformed, leave the
  // existing workspace intact, and release the action for an immediate retry.
  const malformedChooser = await openLayoutChooser(page);
  await malformedChooser.setFiles({
    name: 'broken-layout.pj4.xml',
    mimeType: 'application/xml',
    buffer: Buffer.alloc(0),
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_FAILED name=broken-layout')) || '',
    { timeout: 10000 },
  ).toContain('reason=parse');
  const dialogMessages = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).length;
  await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(dialogMessages);
  const dialog = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).at(-1) || '';
  expect(dialog).toContain('present=1');
  expect(dialog).toContain('broken-layout.pj4.xml');
  await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());

  const afterMalformed = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(afterMalformed.titles).toEqual(original.titles);
  expect(afterMalformed.view).toEqual(original.view);

  const incompatibleChooser = await openLayoutChooser(page);
  await incompatibleChooser.setFiles({
    name: 'missing-data.pj4.xml',
    mimeType: 'application/xml',
    buffer: makeMissingCurveLayout(canonicalLayout),
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_FAILED name=missing-data')) || '',
    { timeout: 15000 },
  ).toContain('reason=apply');
  await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
  const missingDialog = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).at(-1) || '';
  expect(missingDialog).toContain('present=1');
  expect(missingDialog).toContain('Open matching data sources and try again');
  await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());
  const afterIncompatible = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(afterIncompatible.titles).toEqual(original.titles);
  expect(afterIncompatible.view).toEqual(original.view);

  // This target passes curve rebinding, replaces the plot dock during
  // xmlLoadState, and only then fails scene validation. The production restore
  // transaction must reconstruct the original plot rather than leave the
  // partially applied scene workspace behind.
  const lateFailureLayout = makeInvalidSceneLayout(customizeLayout(canonicalLayout, 0.75, 1.5));
  expect(lateFailureLayout.toString('utf8')).toContain('<scene3d>');
  const lateFailureChooser = await openLayoutChooser(page);
  await lateFailureChooser.setFiles({
    name: 'late-scene-failure.pj4.xml',
    mimeType: 'application/xml',
    buffer: lateFailureLayout,
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_FAILED name=late-scene-failure')) || '',
    { timeout: 15000 },
  ).toContain('reason=apply');
  await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
  const lateFailureDialog = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).at(-1) || '';
  expect(lateFailureDialog).toContain('present=1');
  await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());
  const afterLateFailure = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(afterLateFailure.count).toBe(1);
  expect(afterLateFailure.titles).toEqual(original.titles);
  expect(afterLateFailure.view).toEqual(original.view);

  const validChooser = await openLayoutChooser(page);
  await validChooser.setFiles({
    name: 'workspace-one.pj4.xml',
    mimeType: 'application/xml',
    buffer: customizeLayout(canonicalLayout, 0.25, 1.25),
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=workspace-one')) || '',
    { timeout: 15000 },
  ).toContain('workspace-one.pj4.xml');
  const restored = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(restored.count).toBe(1);
  expect(restored.titles).toEqual(['layout-open/temp']);
  expect(restored.style).toBe(1);
  expect(restored.width).toBe(3);
  expect(restored.view.left).toBeCloseTo(0.25, 6);
  expect(restored.view.right).toBeCloseTo(1.25, 6);
  expect(restored.view.bottom).toBeCloseTo(-2, 6);
  expect(restored.view.top).toBeCloseTo(12, 6);
  const restoredDocument = await requestLayoutSnapshot(page, consoleMessages, snapshotMessages);
  snapshotMessages += 1;
  expect(Number(elementAttribute(restoredDocument, 'right_panel_state', 'width'))).toBe(3);
  expect(elementAttribute(restoredDocument, 'right_panel_state', 'style')).toBe('1');
  expect(elementAttribute(restoredDocument, 'left_panel_state', 'sources_tab')).toBe('stream');
  expect(elementAttribute(restoredDocument, 'left_panel_state', 'streaming_buffer')).toBe('37');
  expect(elementAttribute(restoredDocument, 'curve_list_state', 'show_topics')).toBe('false');
  expect(elementAttribute(restoredDocument, 'curve_list_state', 'show_values')).toBe('true');
  expect(elementAttribute(restoredDocument, 'curve_list_state', 'datasets_filter')).toBe('workspace-one-filter');
  // QSplitter may clamp requested pixels to child minimum sizes. Preserve the
  // intended proportion without coupling the contract to one Qt geometry pass.
  expect(splitterRatio(restoredDocument)).toBeCloseTo(360 / 1160, 1);
  expect(Number(elementAttribute(restoredDocument, 'source_timeline', 'zoom'))).toBeCloseTo(0.000001, 12);
  expect(elementAttribute(restoredDocument, 'source_timeline', 'scroll_left_ns')).toBe('500000000');
  expect(elementAttribute(restoredDocument, 'source_timeline', 'snap')).toBe('false');

  // Reopen a second byte-only layout to prove the one-shot picker state was
  // released and the next document, rather than a stale MEMFS path, wins.
  const secondValidChooser = await openLayoutChooser(page);
  await secondValidChooser.setFiles({
    name: 'workspace-two.pj4.xml',
    mimeType: 'application/xml',
    buffer: customizeLayout(canonicalLayout, 0.5, 1.75, 'Lines', '1.5'),
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=workspace-two')) || '',
    { timeout: 15000 },
  ).toContain('workspace-two.pj4.xml');
  const reopened = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(reopened.count).toBe(1);
  expect(reopened.titles).toEqual(['layout-open/temp']);
  expect(reopened.style).toBe(0);
  expect(reopened.width).toBe(1);
  expect(reopened.view.left).toBeCloseTo(0.5, 6);
  expect(reopened.view.right).toBeCloseTo(1.75, 6);
  const reopenedDocument = await requestLayoutSnapshot(page, consoleMessages, snapshotMessages);
  snapshotMessages += 1;
  expect(Number(elementAttribute(reopenedDocument, 'right_panel_state', 'width'))).toBe(1.5);
  expect(elementAttribute(reopenedDocument, 'right_panel_state', 'style')).toBe('0');
  expect(elementAttribute(reopenedDocument, 'left_panel_state', 'streaming_buffer')).toBe('52');
  expect(elementAttribute(reopenedDocument, 'curve_list_state', 'show_topics')).toBe('true');
  expect(elementAttribute(reopenedDocument, 'curve_list_state', 'show_values')).toBe('false');
  expect(elementAttribute(reopenedDocument, 'curve_list_state', 'datasets_filter')).toBe('workspace-two-filter');
  expect(splitterRatio(reopenedDocument)).toBeCloseTo(420 / 1160, 1);
  expect(Number(elementAttribute(reopenedDocument, 'source_timeline', 'zoom'))).toBeCloseTo(0.000002, 12);
  expect(elementAttribute(reopenedDocument, 'source_timeline', 'scroll_left_ns')).toBe('750000000');
  expect(elementAttribute(reopenedDocument, 'source_timeline', 'snap')).toBe('true');

  // Use real File-menu clicks so serialization + saveFileContent run within a
  // trusted browser gesture. Qt's fallback emits a Playwright download event.
  const downloaded = await downloadLayoutFromFileMenu(page, screen, consoleMessages);
  expect(downloaded.fileName).toBe('plotjuggler-layout.pj4.xml');
  expect(downloaded.bytes.equals(reopenedDocument)).toBe(true);
  const downloadedXml = downloaded.bytes.toString('utf8');
  expect(elementAttribute(downloaded.bytes, 'root', 'pj4_version')).toBe(
    elementAttribute(canonicalLayout, 'root', 'pj4_version'),
  );
  expect(elementAttribute(downloaded.bytes, 'root', 'binding')).toBe('generic');
  for (const tag of ['right_panel_state', 'left_panel_state', 'curve_list_state', 'chrome_state', 'source_timeline']) {
    expect(downloadedXml.match(new RegExp(`<${tag}\\b`, 'g')) || []).toHaveLength(1);
  }
  expect(downloadedXml).not.toContain('previouslyLoaded_Datafiles');
  expect(downloadedXml).not.toContain('pj-upload://');
  expect(downloadedXml).not.toContain('/pj_uploads');
  expect(downloadedXml).not.toMatch(/\b\w*dataset_(?:id|source|path)="/);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_LAYOUT_DOWNLOAD_REQUESTED'))).toBe(true);
  expect(await pickerApisRemainExact(page)).toBe(true);

  // Diverge the live workspace, then feed the downloaded bytes back through
  // W9a and require the saved second profile to win again.
  const divergedChooser = await openLayoutChooser(page);
  await divergedChooser.setFiles({
    name: 'workspace-diverged.pj4.xml',
    mimeType: 'application/xml',
    buffer: customizeLayout(canonicalLayout, 0.25, 1.25),
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=workspace-diverged')) || '',
    { timeout: 15000 },
  ).toContain('workspace-diverged.pj4.xml');
  const diverged = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(diverged.style).toBe(1);
  expect(diverged.view.left).toBeCloseTo(0.25, 6);

  const roundTripChooser = await openLayoutChooser(page);
  await roundTripChooser.setFiles({
    name: downloaded.fileName,
    mimeType: 'application/xml',
    buffer: downloaded.bytes,
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=plotjuggler-layout')) || '',
    { timeout: 15000 },
  ).toContain('plotjuggler-layout.pj4.xml');
  const roundTrip = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(roundTrip.count).toBe(1);
  expect(roundTrip.titles).toEqual(['layout-open/temp']);
  expect(roundTrip.style).toBe(0);
  expect(roundTrip.width).toBe(1);
  expect(roundTrip.view.left).toBeCloseTo(0.5, 6);
  expect(roundTrip.view.right).toBeCloseTo(1.75, 6);
  const roundTripDocument = await requestLayoutSnapshot(page, consoleMessages, snapshotMessages);
  snapshotMessages += 1;
  expect(roundTripDocument.equals(downloaded.bytes)).toBe(true);
  expect(elementAttribute(roundTripDocument, 'curve_list_state', 'datasets_filter')).toBe('workspace-two-filter');
  expect(splitterRatio(roundTripDocument)).toBeCloseTo(420 / 1160, 1);
  expect(Number(elementAttribute(roundTripDocument, 'source_timeline', 'zoom'))).toBeCloseTo(0.000002, 12);

  // A second real save proves the scoped capability mask was fully restored
  // and the action does not park on an unobservable completion callback.
  const secondDownload = await downloadLayoutFromFileMenu(page, screen, consoleMessages);
  expect(secondDownload.fileName).toBe('plotjuggler-layout.pj4.xml');
  expect(secondDownload.bytes.equals(roundTripDocument)).toBe(true);

  // Restore an otherwise-valid generic layout whose persisted filter contains
  // a MEMFS path. The live generic serializer demonstrably retains the poison;
  // the download boundary must trip before saveFileContent can emit an artifact.
  const poisonedXml = setElementAttribute(
    roundTripDocument.toString('utf8'),
    'curve_list_state',
    'datasets_filter',
    '/pj_uploads/generic-download-tripwire',
  );
  const poisonedLoads = consoleMessages.filter(
    message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=generic-download-tripwire'),
  ).length;
  const poisonedChooser = await openLayoutChooser(page);
  await poisonedChooser.setFiles({
    name: 'generic-download-tripwire.pj4.xml',
    mimeType: 'application/xml',
    buffer: Buffer.from(poisonedXml),
  });
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=generic-download-tripwire'),
    ).length,
    { timeout: 15000 },
  ).toBeGreaterThan(poisonedLoads);
  const poisonedSnapshot = await requestLayoutSnapshot(page, consoleMessages, snapshotMessages);
  snapshotMessages += 1;
  expect(poisonedSnapshot.toString('utf8')).toContain('/pj_uploads/generic-download-tripwire');

  const failedDownloads = consoleMessages.filter(
    message => message.includes('PJ_WASM_LAYOUT_DOWNLOAD_FAILED'),
  ).length;
  const actionCenter = await saveLayoutActionCenter(page, screen, consoleMessages);
  const unexpectedDownload = page.waitForEvent('download', { timeout: 3000 }).then(() => true, () => false);
  await page.mouse.click(screen.x + actionCenter.x, screen.y + actionCenter.y);
  expect(await unexpectedDownload).toBe(false);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DOWNLOAD_FAILED')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(failedDownloads);

  const tripwireDialogs = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).length;
  await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(tripwireDialogs);
  const tripwireDialog = consoleMessages.filter(
    message => message.includes('PJ_WASM_LAYOUT_DIALOG present='),
  ).at(-1) || '';
  expect(tripwireDialog).toContain('present=1');
  expect(tripwireDialog).toContain('Save Layout');
  expect(tripwireDialog).toContain('ephemeral browser path');
  await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());

  const recentAfter = await requestLayoutPersistence(page, consoleMessages, persistenceMessages + 1);
  expect(recentAfter).toBe(recentBefore);
  const browserPersistence = await requestBrowserPersistenceState(
    page,
    consoleMessages,
    consoleMessages.filter(message => message.includes('PJ_WASM_PERSISTENCE_STATE')).length,
  );
  // Four safe successful generic opens above are retained. The poisoned layout
  // is applied for the tripwire but rejected by persistence; cancelled,
  // malformed, incompatible, and late-failing documents never enter the store.
  expect(browserPersistence.recipes).toBe(4);

  // Drive the production popup with physical input. Reopening the older first
  // recipe must use its stored XML bytes, not the current workspace or a host
  // path, and Clear must remove the durable recipe set.
  const workspaceOneLoads = consoleMessages.filter(
    message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=workspace-one.pj4.xml'),
  ).length;
  await clickRecentMenuAction(page, consoleMessages, 'workspace-one.pj4.xml');
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=workspace-one.pj4.xml'),
    ).length,
    { timeout: 15000 },
  ).toBeGreaterThan(workspaceOneLoads);
  const recentRestore = await requestPlotState(page, consoleMessages, stateMessages);
  stateMessages += 1;
  expect(recentRestore.style).toBe(1);
  expect(recentRestore.width).toBe(3);
  expect(recentRestore.view.left).toBeCloseTo(0.25, 6);
  expect(recentRestore.view.right).toBeCloseTo(1.25, 6);

  await clickRecentMenuAction(page, consoleMessages, 'Clear recent layouts');
  const afterRecentClear = await requestBrowserPersistenceState(
    page,
    consoleMessages,
    consoleMessages.filter(message => message.includes('PJ_WASM_PERSISTENCE_STATE')).length,
  );
  expect(afterRecentClear.recipes).toBe(0);
  expect(await pickerApisRemainExact(page)).toBe(true);
  const actionMessages = consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_ACTION present='));
  expect(actionMessages.length).toBeGreaterThanOrEqual(8);
  expect(actionMessages.every(message => message.includes('present=1 enabled=1 visible=1'))).toBe(true);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_LAYOUT_ACTION_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_FILE_MENU_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_MENU_ACTION_FAILED save_layout'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_RECENT_BUTTON_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_RECENT_MENU_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_LAYOUT_DIALOG_DISMISS_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('source-bound browser layouts download logical references and replay after explicit reselection', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const allConsoleMessages = [];
  let consoleMessages = [];
  observeRuntimeTarget(page, consoleMessages, allConsoleMessages, errors);

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  let screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  const sourceBytes = Buffer.from('time,temp\n0,2\n1,8\n2,4\n');

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'source-reselect.csv',
    mimeType: 'text/csv',
    buffer: sourceBytes,
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_FILE_LOAD_OK')) || '',
    { timeout: 15000 },
  ).toContain('source-reselect/temp:3@');
  { const icon = await placeholderIconCenter(page, screen, consoleMessages, 'plot');
    await page.mouse.click(icon.x, icon.y); }
  await page.waitForTimeout(1500);
  await page.keyboard.press('F9');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_PLOT_FRAME_OK')) || '',
    { timeout: 15000 },
  ).toContain('first_curve=source-reselect/temp');

  const persistenceCount = consoleMessages.filter(
    message => message.includes('PJ_WASM_LAYOUT_PERSISTENCE'),
  ).length;
  const persistenceBefore = await requestLayoutPersistenceState(page, consoleMessages, persistenceCount);
  const downloaded = await downloadSourceLayoutFromFileMenu(page, screen, consoleMessages);
  expect(downloaded.fileName).toBe('plotjuggler-source-layout.pj4.xml');
  const xml = downloaded.bytes.toString('utf8');
  expect(elementAttribute(downloaded.bytes, 'root', 'binding')).toBe('source');
  expect(elementAttribute(downloaded.bytes, 'fileInfo', 'filename')).toBe('source-reselect.csv');
  expect(elementAttribute(downloaded.bytes, 'fileInfo', 'content_sha256')).toBe(
    crypto.createHash('sha256').update(sourceBytes).digest('hex'),
  );
  expect(elementAttribute(downloaded.bytes, 'plugin', 'ID')).toBe('CSV Loader');
  expect(elementAttribute(downloaded.bytes, 'plugin', 'filepath_mode')).toBe('source');
  expect(xml).toContain('"filepath":"source-reselect.csv"');
  expect(xml).toContain('dataset_path="source-reselect.csv"');
  expect(xml.match(/<previouslyLoaded_Datafiles\b/g) || []).toHaveLength(1);
  expect(xml.match(/<fileInfo\b/g) || []).toHaveLength(1);
  expect(xml).not.toContain('pj-upload://');
  expect(xml).not.toContain('/pj_uploads');
  expect(consoleMessages.some(
    message => message.includes('PJ_WASM_SOURCE_LAYOUT_DOWNLOAD_REQUESTED'),
  )).toBe(true);

  // Each independent replay scenario gets a fresh Page target. This destroys
  // staged MEMFS files, upload tokens, and the preceding scenario's native
  // chooser interception state. The browser context remains the same, so the
  // persistence boundary stays realistic and every scenario still enters
  // through physical user gestures.
  let decisionCount = 0;
  const startReplayScenario = async () => {
    ({ page, consoleMessages, screen } = await replaceRuntimeTarget(page, allConsoleMessages, errors));
    await page.evaluate(() => {
      globalThis.__pjWasmPickerApisBefore = {
        open: window.showOpenFilePicker,
        save: window.showSaveFilePicker,
        openOwn: Object.prototype.hasOwnProperty.call(window, 'showOpenFilePicker'),
        saveOwn: Object.prototype.hasOwnProperty.call(window, 'showSaveFilePicker'),
      };
    });
    decisionCount = 0;
  };
  await startReplayScenario();

  const openSavedSourceLayout = async () => {
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
    decisionCount += 1;
  };

  // Cancelling at the decision is a strict no-op and releases the load action.
  await openSavedSourceLayout();
  let geometry = await sourceLayoutDecisionGeometry(page, consoleMessages);
  let cancelledCount = consoleMessages.filter(
    message => message.includes('PJ_WASM_SOURCE_LAYOUT_CANCELLED'),
  ).length;
  await page.mouse.click(screen.x + geometry.cancel.x, screen.y + geometry.cancel.y);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_SOURCE_LAYOUT_CANCELLED')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(cancelledCount);
  const datasetCountMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_DATASET_COUNT count='),
  ).length;
  expect(await requestDatasetCount(page, consoleMessages, datasetCountMessages)).toBe(0);

  // A new target proves cancellation released every callback/state without
  // carrying native chooser interception state into the stale-callback case.
  await startReplayScenario();
  await openSavedSourceLayout();
  geometry = await sourceLayoutDecisionGeometry(page, consoleMessages);
  const sourceChooser = await openSourceReplayChooser(page, screen, geometry, consoleMessages);
  // Supersede the pending generation before delivering its callback. This is
  // the deterministic equivalent of a rapid second click whose browser picker
  // never calls back: the first staged result must be discarded without
  // closing the decision or advancing into import.
  await page.evaluate(() => window.pjWasmSupersedeSourceLayoutPickerProbe());
  await sourceChooser.setFiles({
    name: 'source-reselect.csv',
    mimeType: 'text/csv',
    buffer: sourceBytes,
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_SOURCE_LAYOUT_STALE_PICKER')) || '',
    { timeout: 15000 },
  ).toContain('active=');
  const staleCountMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_DATASET_COUNT count='),
  ).length;
  expect(await requestDatasetCount(page, consoleMessages, staleCountMessages)).toBe(0);
  // The decision remains present and retryable after the stale callback. Run
  // the eventual successful import in its own target so this assertion does
  // not depend on Chromium delivering another native chooser event to a Page
  // target whose preceding interception was deliberately invalidated.
  geometry = await sourceLayoutDecisionGeometry(page, consoleMessages);
  expect(geometry.reselect.x).toBeGreaterThanOrEqual(0);
  expect(geometry.reselect.y).toBeGreaterThanOrEqual(0);

  await startReplayScenario();
  await openSavedSourceLayout();
  geometry = await sourceLayoutDecisionGeometry(page, consoleMessages);
  const replayChooser = await openSourceReplayChooser(page, screen, geometry, consoleMessages);
  await replayChooser.setFiles({
    name: 'source-reselect.csv',
    mimeType: 'text/csv',
    buffer: sourceBytes,
  });
  await expect.poll(
    () => consoleMessages.find(
      message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=plotjuggler-source-layout'),
    ) || '',
    { timeout: 20000 },
  ).toContain('plotjuggler-source-layout.pj4.xml');

  const stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_PLOT_STATE count=')).length;
  const restored = await requestPlotState(page, consoleMessages, stateMessages);
  expect(restored.count).toBe(1);
  expect(restored.titles).toEqual(['source-reselect/temp']);
  const persistenceAfter = await requestLayoutPersistenceState(
    page,
    consoleMessages,
    consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_PERSISTENCE')).length,
  );
  expect(persistenceAfter).toEqual(persistenceBefore);
  const browserPersistence = await requestBrowserPersistenceState(
    page,
    consoleMessages,
    consoleMessages.filter(message => message.includes('PJ_WASM_PERSISTENCE_STATE')).length,
  );
  // Source-bound documents can contain plugin configuration and are therefore
  // explicit downloads only; no implicit localStorage recipe is created.
  expect(browserPersistence.recipes).toBe(0);
  expect(await pickerApisRemainExact(page)).toBe(true);
  expect(allConsoleMessages.some(message => message.includes('PJ_WASM_SOURCE_LAYOUT_RESELECT_FAILED'))).toBe(false);
  expect(allConsoleMessages.some(message => message.includes('PJ_WASM_MENU_ACTION_FAILED save_source_layout'))).toBe(false);
  expect(errors).toEqual([]);
});

test('multi-source browser layouts stage duplicate basenames transactionally and restore exact timeline state', async ({ page }) => {
  test.setTimeout(300000);
  const errors = [];
  const allConsoleMessages = [];
  let consoleMessages = [];
  const observePage = (target, targetMessages) => {
    target.on('pageerror', error => errors.push(String(error)));
    target.on('console', (message) => {
      targetMessages.push(message.text());
      allConsoleMessages.push(message.text());
    });
  };
  observePage(page, consoleMessages);

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  let screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  let decisionCount = 0;
  const openFreshRuntimePage = async () => {
    // A missed native chooser event can leave Chromium's interception state
    // unusable for that Page target even though Qt keeps the decision dialog
    // retryable. Each replay transaction is independent and already crosses a
    // full runtime boundary, so use a new Page target while retaining the same
    // browser context/storage and the exact downloaded layout bytes.
    const context = page.context();
    await page.close();
    consoleMessages = [];
    page = await context.newPage();
    observePage(page, consoleMessages);
    await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
    await waitForQtApp(page, consoleMessages);
    screen = await page.locator('#screen').boundingBox();
    expect(screen).not.toBeNull();
    decisionCount = 0;
  };
  // Same basename AND same schema/source label: post-import shape validation
  // cannot distinguish these files. Their content fingerprints must.
  const leftBytes = Buffer.from('time,value\n0,1\n1,2\n2,3\n');
  const rightBytes = Buffer.from('time,value\n0,8\n1,9\n2,10\n');

  const loadCsv = async (bytes) => {
    const loadedBefore = consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length;
    const chooser = await openFileChooser(page, screen);
    await chooser.setFiles({
      name: 'duplicate.csv',
      mimeType: 'text/csv',
      buffer: bytes,
    });
    await page.waitForTimeout(1000);
    await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
    await expect.poll(
      () => consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length,
      { timeout: 15000 },
    ).toBeGreaterThan(loadedBefore);
  };
  await loadCsv(leftBytes);
  await loadCsv(rightBytes);

  const countMessagesBeforeSave = consoleMessages.filter(
    message => message.includes('PJ_WASM_DATASET_COUNT count='),
  ).length;
  expect(await requestDatasetCount(page, consoleMessages, countMessagesBeforeSave)).toBe(2);
  const downloaded = await downloadSourceLayoutFromFileMenu(page, screen, consoleMessages);
  expect(downloaded.fileName).toBe('plotjuggler-source-layout.pj4.xml');
  expect(elementAttributes(downloaded.bytes, 'fileInfo', 'filename')).toEqual([
    'pj4-sources/0/duplicate.csv',
    'pj4-sources/1/duplicate.csv',
  ]);
  expect(elementAttributes(downloaded.bytes, 'fileInfo', 'content_sha256')).toEqual([
    crypto.createHash('sha256').update(leftBytes).digest('hex'),
    crypto.createHash('sha256').update(rightBytes).digest('hex'),
  ]);
  expect(elementAttributes(downloaded.bytes, 'dataset', 'source_index')).toEqual(['0', '0']);
  expect(downloaded.bytes.toString('utf8')).toContain('"filepath":"pj4-sources/0/duplicate.csv"');
  expect(downloaded.bytes.toString('utf8')).toContain('"filepath":"pj4-sources/1/duplicate.csv"');
  expect(downloaded.bytes.toString('utf8')).not.toContain('pj-upload://');
  expect(downloaded.bytes.toString('utf8')).not.toContain('/pj_uploads');
  const legalNameColumnWidth = elementAttribute(downloaded.bytes, 'source_timeline', 'name_column_width');

  let customizedXml = downloaded.bytes.toString('utf8');
  customizedXml = setElementAttributeAt(customizedXml, 'dataset', 0, 'display_offset_ns', '100000000');
  customizedXml = setElementAttributeAt(customizedXml, 'dataset', 0, 'timeline_order', '1');
  customizedXml = setElementAttributeAt(customizedXml, 'dataset', 1, 'display_offset_ns', '-200000000');
  customizedXml = setElementAttributeAt(customizedXml, 'dataset', 1, 'timeline_order', '0');
  customizedXml = setElementAttribute(customizedXml, 'source_timeline', 'zoom', '0.000003');
  customizedXml = setElementAttribute(customizedXml, 'source_timeline', 'scroll_left_ns', '250000000');
  customizedXml = setElementAttribute(customizedXml, 'source_timeline', 'scroll_top_px', '0');
  customizedXml = setElementAttribute(
    customizedXml,
    'source_timeline',
    'name_column_width',
    legalNameColumnWidth,
  );
  customizedXml = setElementAttribute(customizedXml, 'source_timeline', 'snap', 'false');
  const customized = Buffer.from(customizedXml);

  // Drop both uploads and their staged MEMFS paths before exercising replay.
  await openFreshRuntimePage();

  const openSavedSourceLayout = async (bytes, name) => {
    const layoutChooser = await openLayoutChooser(page);
    await layoutChooser.setFiles({ name, mimeType: 'application/xml', buffer: bytes });
    await expect.poll(
      () => consoleMessages.filter(message => message.includes('PJ_WASM_SOURCE_LAYOUT_DECISION')).length,
      { timeout: 15000 },
    ).toBeGreaterThan(decisionCount);
    decisionCount += 1;
  };
  const selectReplaySource = async (bytes) => {
    const stagedBefore = consoleMessages.filter(
      message => message.includes('PJ_WASM_SOURCE_LAYOUT_STAGED index='),
    ).length;
    const geometry = await sourceLayoutDecisionGeometry(page, consoleMessages);
    const chooser = await openSourceReplayChooser(page, screen, geometry, consoleMessages);
    await chooser.setFiles({ name: 'duplicate.csv', mimeType: 'text/csv', buffer: bytes });
    // setFiles resolves when Chromium populated the input. The Qt callback still
    // has to stage the bytes and advance the decision to the next source; do not
    // click that decision again while the preceding picker callback is in flight.
    await expect.poll(
      () => consoleMessages.filter(
        message => message.includes('PJ_WASM_SOURCE_LAYOUT_STAGED index='),
      ).length,
      { timeout: 15000 },
    ).toBeGreaterThan(stagedBefore);
  };
  const rejectReplaySource = async (bytes) => {
    const rejectedBefore = consoleMessages.filter(
      message => message.includes('PJ_WASM_SOURCE_LAYOUT_SELECTION_REJECTED'),
    ).length;
    const geometry = await sourceLayoutDecisionGeometry(page, consoleMessages);
    const chooser = await openSourceReplayChooser(page, screen, geometry, consoleMessages);
    await chooser.setFiles({ name: 'duplicate.csv', mimeType: 'text/csv', buffer: bytes });
    await expect.poll(
      () => consoleMessages.filter(
        message => message.includes('PJ_WASM_SOURCE_LAYOUT_SELECTION_REJECTED'),
      ).length,
      { timeout: 15000 },
    ).toBeGreaterThan(rejectedBefore);
  };

  // Cancelling after source 1 proves all selections are staged before import:
  // there is no load completion and no partially-created dataset to remove.
  await openSavedSourceLayout(customized, 'multi-source-cancel.pj4.xml');
  const loadCountBeforeCancel = consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length;
  // Selecting source 2's bytes for source 1 is rejected before any import even
  // though both files have the same basename and produce the same topic shape.
  await rejectReplaySource(rightBytes);
  const mismatchDialogMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_LAYOUT_DIALOG present='),
  ).length;
  await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_LAYOUT_DIALOG present=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(mismatchDialogMessages);
  const mismatchDialog = consoleMessages.filter(
    message => message.includes('PJ_WASM_LAYOUT_DIALOG present='),
  ).at(-1) || '';
  expect(mismatchDialog).toContain('not the file saved for source 1 of 2');
  await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());
  await selectReplaySource(leftBytes);
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_SOURCE_LAYOUT_STAGED index=1 total=2')) || '',
    { timeout: 10000 },
  ).toContain('name=duplicate.csv');
  expect(consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length).toBe(loadCountBeforeCancel);
  const stagedCountMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_DATASET_COUNT count='),
  ).length;
  expect(await requestDatasetCount(page, consoleMessages, stagedCountMessages)).toBe(0);
  const cancelGeometry = await sourceLayoutDecisionGeometry(page, consoleMessages);
  const cancelledBefore = consoleMessages.filter(
    message => message.includes('PJ_WASM_SOURCE_LAYOUT_CANCELLED'),
  ).length;
  await page.mouse.click(screen.x + cancelGeometry.cancel.x, screen.y + cancelGeometry.cancel.y);
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_SOURCE_LAYOUT_CANCELLED')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(cancelledBefore);

  // A separate Page target keeps the rollback transaction independent from
  // the deliberate mismatch/cancel chooser sequence above.
  await openFreshRuntimePage();

  // Importing bytes that do not reproduce the saved dataset shape must roll
  // both new sources back, not leave the first successful import behind.
  const shapeFailure = Buffer.from(
    setElementAttributeAt(customizedXml, 'dataset', 0, 'source_name', 'impossible-shape'),
  );
  await openSavedSourceLayout(shapeFailure, 'multi-source-shape-failure.pj4.xml');
  await selectReplaySource(leftBytes);
  await selectReplaySource(rightBytes);
  await expect.poll(
    () => consoleMessages.find(
      message => message.includes('PJ_WASM_LAYOUT_LOAD_FAILED name=multi-source-shape-failure'),
    ) || '',
    { timeout: 30000 },
  ).toContain('reason=source-shape');
  const failedCountMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_DATASET_COUNT count='),
  ).length;
  expect(await requestDatasetCount(page, consoleMessages, failedCountMessages)).toBe(0);
  await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());

  // Start successful replay in a third replay Page target. Retaining the browser
  // context keeps persistence semantics realistic, while replacing the target
  // proves neither failed runtime nor its MEMFS paths are required.
  await openFreshRuntimePage();

  // The clean replay imports in source order, remaps both logical aliases to
  // fresh uploads, and applies the document/timeline exactly once.
  const loadCountBeforeSuccess = consoleMessages.filter(message => message.includes('PJ_FILE_LOAD_OK')).length;
  await openSavedSourceLayout(customized, 'multi-source-success.pj4.xml');
  await selectReplaySource(leftBytes);
  await selectReplaySource(rightBytes);
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=multi-source-success')) || '',
    { timeout: 30000 },
  ).toContain('multi-source-success.pj4.xml');
  const successfulLoads = consoleMessages
    .filter(message => message.includes('PJ_FILE_LOAD_OK'))
    .slice(loadCountBeforeSuccess);
  expect(successfulLoads).toHaveLength(2);
  expect(successfulLoads[0]).toContain('duplicate/value:3@');
  expect(successfulLoads[1]).toContain('duplicate/value:3@');
  const successCountMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_DATASET_COUNT count='),
  ).length;
  expect(await requestDatasetCount(page, consoleMessages, successCountMessages)).toBe(2);

  const roundTrip = await downloadSourceLayoutFromFileMenu(page, screen, consoleMessages);
  expect(elementAttributes(roundTrip.bytes, 'fileInfo', 'filename')).toEqual([
    'pj4-sources/0/duplicate.csv',
    'pj4-sources/1/duplicate.csv',
  ]);
  expect(elementAttributes(roundTrip.bytes, 'fileInfo', 'content_sha256')).toEqual([
    crypto.createHash('sha256').update(leftBytes).digest('hex'),
    crypto.createHash('sha256').update(rightBytes).digest('hex'),
  ]);
  expect(elementAttributes(roundTrip.bytes, 'dataset', 'display_offset_ns')).toEqual([
    '100000000',
    '-200000000',
  ]);
  expect(elementAttributes(roundTrip.bytes, 'dataset', 'timeline_order')).toEqual(['1', '0']);
  expect(Number(elementAttribute(roundTrip.bytes, 'source_timeline', 'zoom'))).toBeCloseTo(0.000003, 12);
  expect(elementAttribute(roundTrip.bytes, 'source_timeline', 'scroll_left_ns')).toBe('250000000');
  expect(elementAttribute(roundTrip.bytes, 'source_timeline', 'scroll_top_px')).toBe('0');
  expect(elementAttribute(roundTrip.bytes, 'source_timeline', 'name_column_width')).toBe(legalNameColumnWidth);
  expect(elementAttribute(roundTrip.bytes, 'source_timeline', 'snap')).toBe('false');
  expect(roundTrip.bytes.toString('utf8')).not.toContain('pj-upload://');
  expect(roundTrip.bytes.toString('utf8')).not.toContain('/pj_uploads');
  expect(allConsoleMessages.some(message => message.includes('PJ_WASM_SOURCE_LAYOUT_RESELECT_FAILED'))).toBe(false);
  expect(allConsoleMessages.some(message => message.includes('PJ_WASM_LAYOUT_DIALOG_DISMISS_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});
