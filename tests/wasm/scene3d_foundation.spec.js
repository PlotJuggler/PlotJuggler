// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const { waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { count, layoutOutcome } = require('./support/console');
const { clickActiveDialogButton } = require('./support/dialogs');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { elementAttribute, setElementAttribute } = require('./support/layout_xml');
const { openFileChooser, openLayoutChooser } = require('./support/pickers');
const { bootScene3D, dropScene3DRows, loadScene3DFixture } = require('./support/scene3d_acceptance');
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

// Loads a layout file; fails as soon as the app reports PJ_WASM_LAYOUT_LOAD_FAILED.
async function uploadLayout(page, messages, name, bytes) {
  const chooser = await openLayoutChooser(page);
  await chooser.setFiles({ name, mimeType: 'application/xml', buffer: bytes });
  await expect.poll(() => layoutOutcome(messages, name), { timeout: 15000 }).not.toBe('');
  expect(layoutOutcome(messages, name)).toContain('PJ_WASM_LAYOUT_LOAD_OK');
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
  await uploadLayout(page, consoleMessages, 'scene3d-foundation.pj4.xml', downloaded.bytes);
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

// Per row between 30% and 90% of the view's height: the grid lines that run toward the camera cross a row as
// short runs of pixels that differ from the background, the lines across the view fill it with one long run.
// Returns the short-run count of each row in the 55-85% band, and how many rows have a long run.
async function gridLineRuns(page, png) {
  return page.evaluate(async (base64) => {
    const image = new Image();
    image.src = `data:image/png;base64,${base64}`;
    await image.decode();
    const canvas = document.createElement('canvas');
    canvas.width = image.width;
    canvas.height = image.height;
    const context = canvas.getContext('2d');
    context.drawImage(image, 0, 0);
    const { data, width, height } = context.getImageData(0, 0, image.width, image.height);
    const histogram = new Map();
    for (let index = 0; index < data.length; index += 4) {
      const key = `${data[index]},${data[index + 1]},${data[index + 2]}`;
      histogram.set(key, (histogram.get(key) || 0) + 1);
    }
    const background = [...histogram.entries()].reduce((best, entry) => (entry[1] > best[1] ? entry : best))[0]
      .split(',').map(Number);
    const differs = (x, y) => background.some(
      (value, channel) => Math.abs(data[(4 * ((y * width) + x)) + channel] - value) > 24,
    );
    const left = Math.round(width * 0.02);
    const right = Math.round(width * 0.98);
    const span = right - left;
    const rowRuns = (y) => {
      const runs = [];
      let run = 0;
      for (let x = left; x <= right; ++x) {
        if (x < right && differs(x, y)) {
          ++run;
        } else {
          if (run > 0) {
            runs.push(run);
          }
          run = 0;
        }
      }
      return runs;
    };
    const short = [];
    let across = 0;
    for (let y = Math.round(height * 0.3); y < Math.round(height * 0.9); ++y) {
      const runs = rowRuns(y);
      if (runs.some(run => run >= span * 0.75)) {
        ++across;
      }
      if (y >= Math.round(height * 0.55) && y < Math.round(height * 0.85)) {
        short.push(runs.filter(run => run < span / 4).length);
      }
    }
    return { short, across };
  }, png.toString('base64'));
}

// A grid line with one end behind the eye must still draw its visible part. Chromium's software WebGL
// (SwiftShader) dropped such lines whole, so a camera inside a line grid drew only the lines across the view.
test('Scene3D line grid draws the lines that run under the camera', async ({ page }) => {
  test.setTimeout(120000);
  const errors = [];
  const consoleMessages = [];
  page.on('pageerror', error => errors.push(String(error)));
  const screen = await bootScene3D(page, consoleMessages);
  await loadScene3DFixture(page, screen, consoleMessages, 'ros2_tf_grid_real.mcap');
  await dropScene3DRows(page, screen, [232]);
  const reportCount = () => count(consoleMessages, 'PJ_WASM_SCENE3D_FOUNDATION_SUMMARY');
  await expect.poll(async () => {
    const state = await requestScene3DFoundationState(page, consoleMessages, reportCount());
    return state.docks[0]?.resolved >= 1;
  }, { timeout: 15000 }).toBe(true);

  // Looking along -x from (0.94, 0, 0.34): the lines along x pass under the camera and behind it.
  const divisions = 40;
  let xml = (await downloadLayoutFromFileMenu(page, screen, consoleMessages)).bytes.toString('utf8');
  const setAttribute = (tag, attribute, value) => {
    xml = setElementAttribute(xml, tag, attribute, value);
    expect(elementAttribute(xml, tag, attribute)).toBe(value.replaceAll('"', '&quot;'));
  };
  setAttribute('scene3d', 'camera_model', 'orbit');
  setAttribute(
    'scene3d', 'camera_state',
    JSON.stringify({ focal: [0, 0, 0], radius: 1, azimuth: 0, elevation: 0.35, perspective: true }),
  );
  setAttribute('scene_controls', 'grid_style', '0');
  setAttribute('scene_controls', 'grid_extent_m', '4');
  setAttribute('scene_controls', 'grid_divisions', String(divisions));
  await uploadLayout(page, consoleMessages, 'scene3d-grid-under-camera.pj4.xml', Buffer.from(xml, 'utf8'));

  // The grid's line vertices: (divisions + 1) lines per axis, 2 axes, 2 vertices per line, plus the TF connections'.
  const gridLineVertices = 2 * 2 * (divisions + 1);
  let state;
  await expect.poll(async () => {
    state = await requestScene3DFoundationState(page, consoleMessages, reportCount());
    const dock = state.docks[0];
    return Math.abs((dock?.camera.radius ?? 0) - 1) < 1e-3 && dock.vertices.lines >= gridLineVertices;
  }, { timeout: 15000 }).toBe(true);
  const { view } = state.docks[0];
  const topLeft = qtPointToCss(screen, state, { x: view.x, y: view.y });
  const bottomRight = qtPointToCss(screen, state, { x: view.x + view.width, y: view.y + view.height });
  const png = await page.screenshot({
    clip: { x: topLeft.x, y: topLeft.y, width: bottomRight.x - topLeft.x, height: bottomRight.y - topLeft.y },
  });
  await test.info().attach('grid-under-camera', { body: png, contentType: 'image/png' });

  const { short, across } = await gridLineRuns(page, png);
  const median = [...short].sort((a, b) => a - b)[Math.floor(short.length / 2)];
  expect(median, `short runs per row: ${short.join(',')}`).toBeGreaterThanOrEqual(4);
  // The lines across the view (about 26 of them are in the 30-90% band) draw whatever happens to the others.
  expect(across, 'rows with a line across the view').toBeGreaterThanOrEqual(8);
  expect(errors).toEqual([]);
});
