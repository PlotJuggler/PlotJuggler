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

const fixture = name => fs.readFileSync(path.resolve(__dirname, 'fixtures', name));

async function boot(page, consoleMessages) {
  consoleMessages.length = 0;
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, consoleMessages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return screen;
}

async function loadFixture(page, screen, consoleMessages, name, browserName = name) {
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name: browserName,
    mimeType: 'application/octet-stream',
    buffer: fixture(name),
  });
  await page.waitForTimeout(1000);
  await clickActiveDialogButton(page, screen, consoleMessages, 'ok');
  // Import and tree population happen after the selection dialog closes.
  await page.waitForTimeout(1000);
}

async function dropRows(page, screen, rows) {
  await page.mouse.click(screen.x + 10, screen.y + 196);
  await page.waitForTimeout(300);
  for (const y of rows) {
    await dragQtCanvas(
      page,
      { x: screen.x + 95, y: screen.y + y },
      { x: screen.x + 820, y: screen.y + 390 },
    );
    await page.waitForTimeout(250);
  }
}

function reportCount(consoleMessages) {
  return consoleMessages.filter(message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY')).length;
}

// One <trail> element exactly as a desktop build writes it (TrailLayer::
// xmlSaveState) for a trail owned by a pose layer. The source-bound attributes
// are deliberate: the browser must restore the trail from the pose layer's own
// topic, never retarget it from the payload.
const DESKTOP_POSE_TRAIL = '<trail source_kind="pose_topic" source_topic_name="/poses"'
  + ' source_dataset_id="1" source_dataset_source="ros2_scene3d_order_real.mcap"'
  + ' past_color="#1f77b4" future_color="#9ecae1" thickness="4"'
  + ' past_visible="1" future_visible="1"/>';

// Turn a browser-saved layout into the desktop-saved shape by nesting that
// trail inside the pose layer's payload. QDom writes childless payloads
// self-closed, but accept the paired spelling too so a serializer detail cannot
// silently turn this into a no-op replacement.
function withDesktopPoseTrail(xml) {
  const selfClosing = /<poses_in_frame\b([^>]*?)\s*\/>/;
  const paired = /<poses_in_frame\b([^>]*)><\/poses_in_frame>/;
  const pattern = selfClosing.test(xml) ? selfClosing : paired;
  expect(xml, 'browser layout has no <poses_in_frame> payload to nest a trail in').toMatch(pattern);
  const desktopXml = xml.replace(pattern, `<poses_in_frame$1>${DESKTOP_POSE_TRAIL}</poses_in_frame>`);
  expect(desktopXml).not.toBe(xml);
  return desktopXml;
}

async function waitForData(page, consoleMessages, predicate, timeout = 30000) {
  let state;
  await expect.poll(async () => {
    state = await requestScene3DFoundationState(page, consoleMessages, reportCount(consoleMessages));
    return state.docks.length === 1 && predicate(state.docks[0].data, state);
  }, { timeout }).toBe(true);
  return state;
}

async function dragTimeline(page, screen, state, toRatio) {
  const slider = state.slider;
  const fromRatio = (slider.value - slider.min) / (slider.max - slider.min);
  const point = ratio => qtPointToCss(screen, state, {
    x: slider.x + 4 + ratio * (slider.width - 8),
    y: slider.y + slider.height / 2,
  });
  await page.mouse.move(point(fromRatio).x, point(fromRatio).y);
  await page.mouse.down({ button: 'left' });
  await page.mouse.move(point(toRatio).x, point(toRatio).y, { steps: 18 });
  await page.mouse.up({ button: 'left' });
}

test('point and depth codecs converge off the browser main thread', async ({ page }) => {
  test.setTimeout(180000);
  const consoleMessages = [];
  const errors = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('console', message => consoleMessages.push(message.text()));
  page.on('pageerror', error => errors.push(String(error)));

  let screen = await boot(page, consoleMessages);
  await loadFixture(page, screen, consoleMessages, 'ros2_compressed_pointcloud_real.mcap');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('type=kCompressedPointCloud')).length,
    { timeout: 30000 },
  ).toBeGreaterThanOrEqual(3);
  await dropRows(page, screen, [212, 232, 252]);
  const compressed = await waitForData(page, consoleMessages, data =>
    data.points.live === 2 && data.points.rendered === 2 && data.points.vertices === 360000 &&
    data.points.compressed === 2 && data.points.decoding === 0 && data.points.completed >= 2);
  expect(compressed.docks[0].data.points.offMain).toBe(compressed.docks[0].data.points.completed);
  expect(compressed.docks[0].data.warnings).toEqual([]);
  expect(compressed.docks[0].readback.rgbSum).toBeGreaterThan(0);

  screen = await boot(page, consoleMessages);
  await loadFixture(page, screen, consoleMessages, 'ros2_depth_cloud_real.mcap');
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('encoding=compressedDepth')).length,
    { timeout: 30000 },
  ).toBeGreaterThanOrEqual(1);
  // Sorted rows: camera_info, color, compressed depth, raw depth, TF, limit.
  await dropRows(page, screen, [305, 283, 261]);
  const depth = await waitForData(page, consoleMessages, data =>
    data.depth.live === 2 && data.depth.vertices === 31419 && data.depth.decoding === 0 &&
    data.depth.completed >= 2);
  expect(depth.docks[0].data.depth.offMain).toBe(depth.docks[0].data.depth.completed);
  expect(depth.docks[0].data.order).toEqual(depth.docks[0].data.submitted);
  expect(depth.docks[0].data.warnings).toEqual([]);
  expect(depth.docks[0].readback.rgbSum).toBeGreaterThan(0);
  expect(errors).toEqual([]);
});

test('occupancy updates and voxel textures stay bounded and seekable', async ({ page }) => {
  test.setTimeout(150000);
  const consoleMessages = [];
  const errors = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('console', message => consoleMessages.push(message.text()));
  page.on('pageerror', error => errors.push(String(error)));

  let screen = await boot(page, consoleMessages);
  await loadFixture(page, screen, consoleMessages, 'ros2_occupancy_real.mcap');
  // Sorted rows: base grid, update stream, TF, over-limit sample.
  await dropRows(page, screen, [256, 216]);
  const initial = await waitForData(page, consoleMessages, data =>
    data.occupancy.live === 1 && data.occupancy.cells === 15360 && data.occupancy.fullUploads >= 1);
  const initialHash = initial.docks[0].readback.frameHash;
  await dragTimeline(page, screen, initial, 1);
  const updated = await waitForData(page, consoleMessages, (data, state) =>
    data.occupancy.partialUploads >= 1 && state.docks[0].readback.frameHash !== initialHash);
  expect(updated.docks[0].data.warnings.every(
    warning => warning.includes('smaller than width*height'),
  )).toBe(true);

  screen = await boot(page, consoleMessages);
  await loadFixture(page, screen, consoleMessages, 'foxglove_voxel_real.mcap');
  await dropRows(page, screen, [232]);
  const voxels = await waitForData(page, consoleMessages, data =>
    data.voxels.live === 1 && data.voxels.count === 5184 && data.voxels.uploads >= 1);
  expect(voxels.docks[0].data.voxels.max3d).toBeGreaterThanOrEqual(256);
  expect(voxels.docks[0].data.warnings).toEqual([]);
  expect(voxels.docks[0].readback.rgbSum).toBeGreaterThan(0);
  expect(errors).toEqual([]);
});

test('a desktop-saved pose trail replays and draws in the browser', async ({ page }) => {
  test.setTimeout(150000);
  const consoleMessages = [];
  const errors = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('console', message => consoleMessages.push(message.text()));
  page.on('pageerror', error => errors.push(String(error)));

  const screen = await boot(page, consoleMessages);
  await loadFixture(page, screen, consoleMessages, 'ros2_scene3d_order_real.mcap');
  // Sorted rows: point, poses, TF, occupancy, second point.
  await dropRows(page, screen, [212, 232, 252, 283, 302]);
  const live = await waitForData(page, consoleMessages, data =>
    data.points.live === 2 && data.poses.live === 1 && data.poses.arms === 768);
  expect(live.docks[0].data.trails.rendered).toBe(0);

  const downloaded = await downloadLayoutFromFileMenu(page, screen, consoleMessages);
  const browserXml = downloaded.bytes.toString('utf8');
  // The trail is opt-in, so the browser's own layout has none; the nested
  // payload below is exactly what a desktop session writes.
  expect(browserXml).not.toMatch(/<trail\b/);
  const desktopXml = withDesktopPoseTrail(browserXml);
  expect([...desktopXml.matchAll(/<trail\b/g)]).toHaveLength(1);

  const layoutChooser = await openLayoutChooser(page);
  await layoutChooser.setFiles({
    name: 'scene3d-desktop-trail.pj4.xml',
    mimeType: 'application/xml',
    buffer: Buffer.from(desktopXml, 'utf8'),
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=scene3d-desktop-trail')) || '',
    { timeout: 15000 },
  ).toContain('scene3d-desktop-trail.pj4.xml');

  // The identity check belongs INSIDE the predicate: a rejected pose payload
  // would leave the pre-existing dock standing, and every count below would
  // then pass against the dock the drops built rather than the replayed one.
  const restored = await waitForData(page, consoleMessages, (data, state) =>
    state.docks[0].identity !== live.docks[0].identity
    && data.poses.live === 1 && data.poses.arms === 768
    && data.trails.rendered === 1 && data.trails.vertices >= 4);
  expect(restored.docks[0].data.warnings).toEqual([]);
  expect(errors).toEqual([]);
});

test('heterogeneous physical order survives a generic layout replay', async ({ page }) => {
  test.setTimeout(150000);
  const consoleMessages = [];
  const errors = [];
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('console', message => consoleMessages.push(message.text()));
  page.on('pageerror', error => errors.push(String(error)));

  const screen = await boot(page, consoleMessages);
  await loadFixture(page, screen, consoleMessages, 'ros2_scene3d_order_real.mcap');
  // Sorted rows: point, poses, TF, occupancy, second point.
  await dropRows(page, screen, [212, 232, 252, 283, 302]);
  let state = await waitForData(page, consoleMessages, data =>
    data.points.live === 2 && data.points.rendered === 2 && data.poses.live === 1 &&
    data.poses.arms === 768 && data.occupancy.live === 1 && data.occupancy.cells === 6400);
  expect(state.docks[0].data.order).toEqual(state.docks[0].data.submitted);
  const initialOrder = state.docks[0].data.order;
  expect(initialOrder).toHaveLength(4);

  const toggle = state.docks[0].data.rightPanel.toggle;
  await page.mouse.click(
    screen.x + toggle.x + toggle.width / 2,
    screen.y + toggle.y + toggle.height / 2,
  );
  state = await waitForData(page, consoleMessages, data =>
    data.rightPanel.visible && data.rows.length === 4);
  const list = state.docks[0].data.list;
  // The Qt/WASM backing surface can be wider than its CSS canvas. The docked
  // list is right-anchored, so recover its visible CSS left edge from the
  // canvas edge while retaining its stable row geometry.
  const rowX = screen.x + screen.width - list.width + Math.min(24, list.width * 0.2);
  await dragQtCanvas(
    page,
    { x: rowX, y: screen.y + list.y + 50 },
    { x: rowX, y: screen.y + list.y - 8 },
  );
  const reordered = [initialOrder[2], initialOrder[0], initialOrder[1], initialOrder[3]];
  state = await waitForData(page, consoleMessages, data =>
    JSON.stringify(data.order) === JSON.stringify(reordered) &&
    JSON.stringify(data.submitted) === JSON.stringify(reordered));

  const downloaded = await downloadLayoutFromFileMenu(page, screen, consoleMessages);
  const xml = downloaded.bytes.toString('utf8');
  expect(xml).not.toMatch(/<layer\b[^>]*dataset_(?:id|source|path)=/);
  expect([...xml.matchAll(/<layer\b[^>]*topic_name="([^"]+)"/g)].map(match => match[1]))
    .toEqual(['/zz_grid', '/points', '/poses', '/zz_points']);

  const layoutChooser = await openLayoutChooser(page);
  await layoutChooser.setFiles({
    name: 'scene3d-data-generic.pj4.xml',
    mimeType: 'application/xml',
    buffer: downloaded.bytes,
  });
  await expect.poll(
    () => consoleMessages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=scene3d-data-generic')) || '',
    { timeout: 15000 },
  ).toContain('scene3d-data-generic.pj4.xml');
  const restored = await waitForData(page, consoleMessages, data =>
    JSON.stringify(data.order) === JSON.stringify(reordered) &&
    JSON.stringify(data.submitted) === JSON.stringify(reordered));
  expect(restored.docks[0].data.points).toMatchObject({ live: 2, rendered: 2 });
  expect(restored.docks[0].data.poses).toMatchObject({ live: 1, arms: 768 });
  expect(restored.docks[0].data.occupancy).toMatchObject({ live: 1, cells: 6400 });
  expect(restored.docks[0].data.warnings).toEqual([]);
  expect(errors).toEqual([]);
});
