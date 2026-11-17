// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const { waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { clickActiveDialogButton } = require('./support/dialogs');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { openFileChooser, openLayoutChooser } = require('./support/pickers');
const { qtPointToCss, requestScene3DFoundationState } = require('./support/scene3d_foundation');

async function dragTimeline(page, screen, state, fromRatio, toRatio) {
  const point = ratio => qtPointToCss(screen, state, {
    x: state.slider.x + 4 + (ratio * (state.slider.width - 8)),
    y: state.slider.y + (state.slider.height / 2),
  });
  await page.mouse.move(point(fromRatio).x, point(fromRatio).y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(point(toRatio).x, point(toRatio).y, { steps: 18 });
  await page.mouse.up({ button: 'left' });
  await page.waitForTimeout(300);
}

async function selectNextComboItem(page, screen, state, control) {
  const center = qtPointToCss(screen, state, {
    x: control.x + (control.width / 2),
    y: control.y + (control.height / 2),
  });
  await page.mouse.click(center.x, center.y);
  await page.keyboard.press('ArrowDown');
  await page.keyboard.press('Enter');
  await page.waitForTimeout(200);
}

test('Scene3D TF/grid renders, responds to input, and replays a generic layout', async ({ page }) => {
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

  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: 'tf-grid.mcap',
    mimeType: 'application/octet-stream',
    buffer: fs.readFileSync(path.resolve(__dirname, 'fixtures', 'ros2_tf_grid_real.mcap')),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_ROS_OBJECT_OK topic=/tf')) || '',
    { timeout: 30000 },
  ).toContain('type=kFrameTransforms');

  await page.mouse.click(screen.x + 10, screen.y + 192);
  await dragQtCanvas(
    page,
    { x: screen.x + 95, y: screen.y + 232 },
    { x: screen.x + 820, y: screen.y + 390 },
  );

  const reportCount = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY'),
  ).length;
  let initial;
  await expect.poll(async () => {
    initial = await requestScene3DFoundationState(page, consoleMessages, reportCount());
    const dock = initial.docks[0];
    return dock?.resolved >= 1 && dock.vertices.lines >= 50 && dock.readback.width > 600;
  }, { timeout: 15000 }).toBe(true);

  expect(initial.docks).toHaveLength(1);
  const initialDock = initial.docks[0];
  expect(initialDock).toMatchObject({ visible: true, fixed: 'map', auto: true, model: 0 });
  expect(initialDock.frames).toEqual(expect.arrayContaining(['map', 'odom']));
  expect(initialDock.vertices.triangles).toBe(0);
  expect(initial.slider).toMatchObject({ enabled: true });
  expect(initial.slider.max).toBeGreaterThan(initial.slider.min);

  // Moving the timeline changes TF geometry without replacing the widget.
  const initialRatio = (initial.slider.value - initial.slider.min) / (initial.slider.max - initial.slider.min);
  const seekRatio = initialRatio < 0.5 ? 1 : 0;
  await dragTimeline(page, screen, initial, initialRatio, seekRatio);
  const sought = await requestScene3DFoundationState(page, consoleMessages, reportCount());
  expect(sought.docks[0].identity).toBe(initialDock.identity);
  expect(sought.docks[0].readback.frameHash).not.toBe(initialDock.readback.frameHash);

  const center = qtPointToCss(screen, sought, {
    x: sought.docks[0].view.x + (sought.docks[0].view.width / 2),
    y: sought.docks[0].view.y + (sought.docks[0].view.height / 2),
  });
  await page.mouse.move(center.x, center.y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(center.x + 90, center.y - 45, { steps: 12 });
  await page.mouse.up({ button: 'left' });
  const orbited = await requestScene3DFoundationState(page, consoleMessages, reportCount());
  expect(orbited.docks[0].camera.azimuth).not.toBeCloseTo(sought.docks[0].camera.azimuth, 4);
  expect(orbited.docks[0].camera.elevation).not.toBeCloseTo(sought.docks[0].camera.elevation, 4);

  await page.mouse.move(center.x, center.y);
  await page.mouse.wheel(0, -600);
  await page.waitForTimeout(200);
  const zoomed = await requestScene3DFoundationState(page, consoleMessages, reportCount());
  expect(zoomed.docks[0].camera.radius).not.toBeCloseTo(orbited.docks[0].camera.radius, 4);

  await selectNextComboItem(page, screen, zoomed, zoomed.docks[0].controls.fixed);
  const fixedChanged = await requestScene3DFoundationState(page, consoleMessages, reportCount());
  expect(fixedChanged.docks[0]).toMatchObject({ fixed: 'odom', auto: false });
  await selectNextComboItem(page, screen, fixedChanged, fixedChanged.docks[0].controls.camera);
  const configured = await requestScene3DFoundationState(page, consoleMessages, reportCount());
  expect(configured.docks[0].model).toBe(1);

  const downloaded = await downloadLayoutFromFileMenu(page, screen, consoleMessages);
  const xml = downloaded.bytes.toString('utf8');
  expect(xml).toMatch(/<scene3d\b[^>]*camera_model="xy_orbit"/);
  expect(xml).toMatch(/<config_topic\b[^>]*topic_name="\/tf"/);
  expect(xml).not.toMatch(/<config_topic\b[^>]*dataset_(?:id|source|path)=/);
  expect(xml).not.toContain('pj-upload://');
  expect(xml).not.toContain('/pj_uploads/');

  // Diverge the controls; a generic replay must build a new dock and rebind /tf uniquely, with no persisted
  // browser dataset identity.
  await selectNextComboItem(page, screen, configured, configured.docks[0].controls.fixed);
  await selectNextComboItem(page, screen, configured, configured.docks[0].controls.camera);
  const layoutChooser = await openLayoutChooser(page);
  await layoutChooser.setFiles({
    name: 'scene3d-foundation.pj4.xml',
    mimeType: 'application/xml',
    buffer: downloaded.bytes,
  });
  await expect.poll(
    () => consoleMessages.find(
      message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=scene3d-foundation'),
    ) || '',
    { timeout: 15000 },
  ).toContain('scene3d-foundation.pj4.xml');
  const restored = await requestScene3DFoundationState(page, consoleMessages, reportCount());
  expect(restored.docks).toHaveLength(1);
  expect(restored.docks[0].identity).not.toBe(configured.docks[0].identity);
  expect(restored.docks[0]).toMatchObject({ fixed: 'odom', auto: false, model: 1 });
  for (const key of ['focalX', 'focalY', 'focalZ', 'radius', 'azimuth', 'elevation', 'orthoScale']) {
    expect(restored.docks[0].camera[key]).toBeCloseTo(configured.docks[0].camera[key], 4);
  }

  expect(consoleMessages.some(message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_FAILED'))).toBe(false);
  expect(consoleMessages.some(message => message.includes('QRhiWidget: No QRhi'))).toBe(false);
  expect(errors).toEqual([]);
});
