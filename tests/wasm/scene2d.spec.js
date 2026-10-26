// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const crypto = require('crypto');
const { waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { clickActiveDialogButton } = require('./support/dialogs');
const { base64Fixture } = require('./support/fixtures');
const { openFileChooser } = require('./support/pickers');
const { requestMultiPlotState } = require('./support/plot_probes');
const { requestScene2DState } = require('./support/scene2d');

function ros2CompressedImageFixture() {
  return base64Fixture('ros2_compressed_image.mcap.b64');
}

function foxgloveScene2DFixture() {
  return base64Fixture('foxglove_scene2d.mcap.b64');
}

function foxgloveVideoFixture() {
  return base64Fixture('foxglove_video.mcap.b64');
}

async function dragTimelineSlider(page, screen, slider, fromRatio, toRatio) {
  const usableWidth = slider.width - 8;
  const point = ratio => ({
    x: screen.x + slider.x + 4 + (ratio * usableWidth),
    y: screen.y + slider.y + (0.5 * slider.height),
  });
  const from = point(fromRatio);
  const to = point(toRatio);
  await page.mouse.move(from.x, from.y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(to.x, to.y, { steps: 18 });
  await page.mouse.up({ button: 'left' });
  await page.waitForTimeout(350);
}

async function splitDockHorizontally(page, screen, consoleMessages, center) {
  await page.mouse.click(center.x, center.y, { button: 'right' });
  await page.waitForTimeout(250);
  const previous = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=split_horizontal'),
  ).length;
  await page.evaluate(() => window.pjWasmReportSplitHorizontalActionProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_MENU_ACTION name=split_horizontal')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previous);
  const menuMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=split_horizontal'),
  ).at(-1) || '';
  const actionCenter = menuMessage.match(/center=(\d+),(\d+)/);
  expect(actionCenter, `unparseable split action: ${menuMessage}`).not.toBeNull();
  await page.mouse.click(screen.x + Number(actionCenter[1]), screen.y + Number(actionCenter[2]));
}

test('official ROS2 compressed images render and seek in Scene2D', async ({ page }) => {
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

  const fixture = ros2CompressedImageFixture();
  expect(crypto.createHash('sha256').update(fixture).digest('hex')).toBe(
    '5e6ff63c3838d3018caecf815dfc5466919b9151e3d29c988e5bded3e7b6d8ca',
  );
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'ros2-compressed-image.mcap',
    mimeType: 'application/octet-stream',
    buffer: fixture,
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(
      message => message.includes('PJ_WASM_ROS_OBJECT_OK topic=/camera/image/compressed'),
    ) || '',
    { timeout: 30000 },
  ).toContain('entries=3 type=kImage');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 212 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await page.waitForTimeout(1000);

  let stateMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE2D_SUMMARY'),
  ).length;
  const initial = await requestScene2DState(page, consoleMessages, stateMessages);
  expect(initial.docks).toHaveLength(1);
  expect(initial.docks[0]).toMatchObject({ layers: 1, visible: true });
  expect(initial.docks[0].viewer.width).toBeGreaterThan(700);
  expect(initial.docks[0].viewer.height).toBeGreaterThan(500);
  expect(initial.docks[0].readback.framebuffer.width).toBeGreaterThan(700);
  expect(initial.docks[0].readback.framebuffer.height).toBeGreaterThan(500);
  expect(initial.docks[0].readback.red).toBeGreaterThan(100000);
  expect(initial.docks[0].readback.blue).toBeGreaterThan(100000);
  expect(initial.docks[0].readback.green).toBe(0);
  expect(initial.docks[0].readback.magenta).toBe(0);
  expect(initial.slider).toMatchObject({ min: 0, max: 2, enabled: true });
  expect(Math.abs(initial.slider.value)).toBeLessThan(0.01);

  await dragTimelineSlider(page, screen, initial.slider, 0, 0.5);
  stateMessages += 1;
  const middle = await requestScene2DState(page, consoleMessages, stateMessages);
  expect(Math.abs(middle.slider.value - 1)).toBeLessThan(0.01);
  expect(middle.docks[0].readback.green).toBeGreaterThan(100000);
  expect(middle.docks[0].readback.magenta).toBeGreaterThan(100000);
  expect(middle.docks[0].readback.red).toBe(0);
  expect(middle.docks[0].readback.blue).toBe(0);

  await dragTimelineSlider(page, screen, middle.slider, 0.5, 1);
  stateMessages += 1;
  const final = await requestScene2DState(page, consoleMessages, stateMessages);
  expect(Math.abs(final.slider.value - 2)).toBeLessThan(0.01);
  expect(final.docks[0].readback.yellow).toBeGreaterThan(100000);
  expect(final.docks[0].readback.cyan).toBeGreaterThan(100000);
  expect(final.docks[0].readback.green).toBe(0);
  expect(final.docks[0].readback.magenta).toBe(0);

  expect(consoleMessages.some(message => message.includes('QRhiWidget: No QRhi'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('Wrong JPEG library version'))).toBe(false);
  expect(consoleMessages.some(message => /MediaViewerWidget: failed|PJ_WASM_SCENE2D_FAILED/.test(message))).toBe(false);
  expect(errors).toEqual([]);
});

test('Foxglove JPEG, annotations, split Scene2D docks, and recreation render', async ({ page }) => {
  test.setTimeout(180000);
  const errors = [];
  const consoleMessages = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('pageerror', error => errors.push(String(error)));
  page.on('console', message => consoleMessages.push(message.text()));

  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();

  const fixture = foxgloveScene2DFixture();
  expect(crypto.createHash('sha256').update(fixture).digest('hex')).toBe(
    '813ea92088ef6f3e37505d41cf97b08e620887a4021f99d78f7a1d54ba47a1f4',
  );
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'foxglove-scene2d.mcap',
    mimeType: 'application/octet-stream',
    buffer: fixture,
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  for (const [topic, type] of [
    ['/foxglove/jpeg/a', 'kImage'],
    ['/foxglove/jpeg/b', 'kImage'],
    ['/foxglove/annotations/a', 'kImageAnnotations'],
  ]) {
    await expect.poll(
      () => consoleMessages.find(
        message => message.includes(`PJ_WASM_PROTOBUF_OBJECT_OK topic=${topic}`),
      ) || '',
      { timeout: 30000 },
    ).toContain(`entries=3 type=${type}`);
  }

  // The dataset's three sorted rows are annotations, JPEG A, and JPEG B.
  // Every drop travels through CurveTreeView's production MIME path.
  await page.mouse.click(screen.x + 10, screen.y + 192);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );
  await page.waitForTimeout(1000);

  const sceneCount = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE2D_SUMMARY'),
  ).length;
  let first = await requestScene2DState(page, consoleMessages, sceneCount());
  expect(first.docks).toHaveLength(1);
  expect(first.docks[0].layers).toBe(1);
  expect(first.docks[0].readback.red).toBeGreaterThan(100000);
  expect(first.docks[0].readback.blue).toBeGreaterThan(100000);
  expect(first.docks[0].readback.green).toBe(0);
  const whiteBeforeOverlay = first.docks[0].readback.white;

  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 212 },
    {
      x: screen.x + first.docks[0].viewer.x + (first.docks[0].viewer.width / 2),
      y: screen.y + first.docks[0].viewer.y + (first.docks[0].viewer.height / 2),
    },
  );
  await page.waitForTimeout(750);
  first = await requestScene2DState(page, consoleMessages, sceneCount());
  expect(first.docks).toHaveLength(1);
  expect(first.docks[0].layers).toBe(2);
  expect(first.docks[0].readback.white).toBeGreaterThan(whiteBeforeOverlay + 1000);

  await splitDockHorizontally(
    page,
    screen,
    consoleMessages,
    {
      x: screen.x + first.docks[0].viewer.x + (first.docks[0].viewer.width / 2),
      y: screen.y + first.docks[0].viewer.y + (first.docks[0].viewer.height / 2),
    },
  );

  let multiCount = consoleMessages.filter(message => message.includes('PJ_WASM_MULTI_SUMMARY')).length;
  let splitLayout;
  const splitDeadline = Date.now() + 10000;
  do {
    splitLayout = await requestMultiPlotState(page, consoleMessages, multiCount);
    multiCount += 1;
    if (splitLayout.docks.length === 2 && splitLayout.docks[1].rect.width > 200) {
      break;
    }
    await page.waitForTimeout(100);
  } while (Date.now() < splitDeadline);
  expect(splitLayout.docks).toHaveLength(2);
  const placeholder = splitLayout.docks[1];

  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 252 },
    {
      x: screen.x + placeholder.rect.x + (placeholder.rect.width / 2),
      y: screen.y + placeholder.rect.y + (placeholder.rect.height / 2),
    },
  );

  const settledScene = async (deadlineMs = 15000) => {
    let state;
    const deadline = Date.now() + deadlineMs;
    do {
      state = await requestScene2DState(page, consoleMessages, sceneCount());
      if (state.docks.length === 2 && state.docks.every(
        dock => dock.visible && dock.viewer.width > 200 && dock.viewer.height > 300 &&
          dock.readback.framebuffer.width > 200,
      )) {
        return state;
      }
      await page.waitForTimeout(100);
    } while (Date.now() < deadline);
    return state;
  };

  let populated = await settledScene();
  expect(populated.docks).toHaveLength(2);
  expect(populated.docks[0].layers).toBe(2);
  expect(populated.docks[0].readback.red).toBeGreaterThan(30000);
  expect(populated.docks[0].readback.blue).toBeGreaterThan(30000);
  expect(populated.docks[0].readback.white).toBeGreaterThan(500);
  expect(populated.docks[1].layers).toBe(1);
  expect(populated.docks[1].readback.green).toBeGreaterThan(30000);
  expect(populated.docks[1].readback.magenta).toBeGreaterThan(30000);
  expect(populated.docks[0].readback.white).toBeGreaterThan(populated.docks[1].readback.white + 1000);
  const populatedIdentities = populated.docks.map(dock => dock.identity);

  // Resize through the real ADS splitter and require both QRhiWidget surfaces
  // to repaint at their new framebuffer sizes.
  multiCount = consoleMessages.filter(message => message.includes('PJ_WASM_MULTI_SUMMARY')).length;
  const beforeResize = await requestMultiPlotState(page, consoleMessages, multiCount);
  const rightDock = beforeResize.docks[1];
  const splitX = screen.x + rightDock.rect.x - 1;
  const splitY = screen.y + rightDock.rect.y + (rightDock.rect.height / 2);
  await page.mouse.move(splitX, splitY);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(splitX + 90, splitY, { steps: 18 });
  await page.mouse.up({ button: 'left' });
  await page.waitForTimeout(750);

  const resized = await settledScene();
  expect(resized.docks[0].viewer.width).toBeGreaterThan(populated.docks[0].viewer.width + 50);
  expect(resized.docks[1].viewer.width).toBeLessThan(populated.docks[1].viewer.width - 50);
  expect(resized.docks[0].readback.white).toBeGreaterThan(500);
  expect(resized.docks[1].readback.green).toBeGreaterThan(20000);

  // Undo applies a serialized workspace snapshot and reconstructs both Scene2D
  // docks. The browser must release/recreate their QRhi resources and still
  // render the independent image and annotation layers.
  await page.keyboard.press('Control+z');
  await page.waitForTimeout(1000);
  const undone = await settledScene();
  expect(undone.docks.every(dock => !populatedIdentities.includes(dock.identity))).toBe(true);
  expect(undone.docks[0].readback.red).toBeGreaterThan(30000);
  expect(undone.docks[0].readback.white).toBeGreaterThan(500);
  expect(undone.docks[1].readback.green).toBeGreaterThan(30000);

  // Capture immediately: this must not rely on the screenshot request itself
  // causing a later compositor refresh.
  const finalImage = await page.screenshot();
  // Exact QRhi readbacks above validate every dock's decoded content; this
  // full-window golden only guards the browser compositor integration.
  expect(finalImage).toMatchSnapshot('wasm-scene2d-foxglove-matrix.png', { maxDiffPixelRatio: 0.05 });

  expect(consoleMessages.some(message => message.includes('PJ_WASM_PROTOBUF_OBJECT_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('QRhiWidget: No QRhi'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('Wrong JPEG library version'))).toBe(false);
  expect(consoleMessages.some(message => /JPEG (header|decode) failed|MediaViewerWidget: failed/.test(message))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_SCENE2D_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});

test('Foxglove H.264 video decodes, renders and seeks in Scene2D', async ({ page }) => {
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

  // 60 Annex B access units of pj_scene2D/testdata/test_h264_bframes.mp4
  // (128x128, B-frames), one foxglove.CompressedVideo message every 100 ms.
  const fixture = foxgloveVideoFixture();
  expect(crypto.createHash('sha256').update(fixture).digest('hex')).toBe(
    '7b12d7a1b0cff39abefccbca6e8dceffa88cb7fd41262182971a301ef1d342c6',
  );
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'foxglove-video.mcap',
    mimeType: 'application/octet-stream',
    buffer: fixture,
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');

  await expect.poll(
    () => consoleMessages.find(
      message => message.includes('PJ_WASM_PROTOBUF_OBJECT_OK topic=/foxglove/video'),
    ) || '',
    { timeout: 30000 },
  ).toContain('entries=60 type=kVideoFrame');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 212 },
    { x: screen.x + 820, y: screen.y + 390 },
  );

  // The first frame arrives once the software decoder has produced it; poll
  // rather than sleep so a slow decode worker does not make the check flaky.
  let stateMessages = consoleMessages.filter(message => message.includes('PJ_WASM_SCENE2D_SUMMARY')).length;
  let first;
  await expect.poll(async () => {
    first = await requestScene2DState(page, consoleMessages, stateMessages);
    stateMessages += 1;
    return first.docks.length === 1 ? first.docks[0].readback.yellow : 0;
  }, { timeout: 30000 }).toBeGreaterThan(5000);
  expect(first.docks[0]).toMatchObject({ layers: 1, visible: true });
  expect(first.docks[0].readback.white).toBeGreaterThan(5000);
  expect(first.slider.enabled).toBe(true);
  expect(first.slider.max).toBeGreaterThan(5.8);

  // Seek past the middle: decoding forward from the preceding keyframe must
  // still land on a real picture.
  await dragTimelineSlider(page, screen, first.slider, 0, 0.6);
  let middle;
  await expect.poll(async () => {
    middle = await requestScene2DState(page, consoleMessages, stateMessages);
    stateMessages += 1;
    return middle.slider.value;
  }, { timeout: 15000 }).toBeGreaterThan(3);
  await expect.poll(async () => {
    middle = await requestScene2DState(page, consoleMessages, stateMessages);
    stateMessages += 1;
    return middle.docks[0].readback.yellow;
  }, { timeout: 15000 }).toBeGreaterThan(5000);

  expect(consoleMessages.some(message => message.includes('QRhiWidget: No QRhi'))).toBe(false);
  expect(consoleMessages.some(message => /MediaViewerWidget: failed|PJ_WASM_SCENE2D_FAILED/.test(message))).toBe(false);
  expect(consoleMessages.some(message => message.includes('PJ_WASM_PROTOBUF_OBJECT_FAILED'))).toBe(false);
  expect(errors).toEqual([]);
});
