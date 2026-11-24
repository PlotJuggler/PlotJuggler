// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { openLayoutChooser, openRobotChooser } = require('./support/pickers');
const {
  bootScene3D: boot,
  clickScene3DControl: clickQtControl,
  dropScene3DRows: dropRows,
  fixture,
  loadScene3DFixture: loadFixture,
  waitForScene3DModels: waitForModels,
} = require('./support/scene3d_acceptance');

test('SceneEntities submits procedural geometry and all five PBR maps', async ({ page, browserName }) => {
  test.setTimeout(90000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const externalModel = fs.readFileSync(path.resolve(
    __dirname,
    '../../pj_scene3D/widgets/tests/fixtures/meshes/embedded_base.glb',
  ));
  await page.route('https://models.plotjuggler.test/embedded_base.glb', route => route.fulfill({
    status: 200,
    contentType: 'model/gltf-binary',
    headers: { 'access-control-allow-origin': '*' },
    body: externalModel,
  }));

  const screen = await boot(page, messages);
  await loadFixture(page, screen, messages, 'foxglove_scene_entities_real.mcap');
  // Sorted rows: /markers, /tf/a, /tf/b, /zz_too_many.
  await dropRows(page, screen, [232, 212]);

  const active = await waitForModels(page, messages, models =>
    models.markers.live === 1 && models.markers.decoding === 0 &&
    models.markers.rendered === 1 && models.models.ready >= 1 &&
    models.models.draws >= 1);
  const models = active.docks[0].models;
  expect(models.markers.offMain).toBe(models.markers.completed);
  expect(models.markers.families).toMatchObject({
    spheres: 2, cylinders: 1, arrows: 1, lineVertices: 8, triangleVertices: 6,
  });
  expect(models.markers.skipped).toMatchObject({ text: 1, models: 1, invalid: 0 });
  expect(models.models.bytes).toBeGreaterThan(0);
  expect(models.models.maps.baseColor).toBeGreaterThanOrEqual(1);
  expect(models.models.maps).toMatchObject({
    metallicRoughness: 1, normal: 1, occlusion: 1, emissive: 1,
  });
  expect(active.docks[0].readback.rgbSum).toBeGreaterThan(0);
  // Headless Chromium has no GPU (SwiftShader): the view must take the software path, whose first HDR-chain frame held
  // the main thread for seconds. Firefox's renderer follows the runner, so its outcome is only logged.
  const hdr = models.quality.hdr;
  console.log(`PJ_WASM_SCENE3D_SOFTWARE browser=${browserName} hdr_active=${hdr.active} reason=${hdr.error}`);
  if (browserName === 'chromium') {
    expect(hdr.active).toBe(false);
    expect(hdr.error).toMatch(/^Software rendering \(SwiftShader\)/);
  }

  expect(messages.some(message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_FAILED'))).toBe(false);
  expect(messages.some(message => message.includes('QRhiWidget: No QRhi'))).toBe(false);
  expect(errors).toEqual([]);
});

test('a local URDF renders from browser-granted bytes and replays without claiming file access', async ({ page }) => {
  test.setTimeout(90000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const screen = await boot(page, messages);
  await loadFixture(page, screen, messages, 'ros2_robot_description_real.mcap');
  // Sorted rows: /robot_description, /tf. TF creates the dock and map frame.
  await dropRows(page, screen, [232]);

  let state = await waitForModels(page, messages, (_models, dock) => dock.fixed === 'map');
  await clickQtControl(page, screen, state, state.docks[0].data.rightPanel.toggle);
  state = await waitForModels(page, messages, models =>
    models.robots.sourceIndex >= 0);
  expect(state.docks[0].models.robots.sourceIndex).toBe(0);

  const robotChooser = await openRobotChooser(page);
  await robotChooser.setFiles({
    name: 'browser_robot.urdf',
    mimeType: 'application/xml',
    buffer: fixture('browser_robot.urdf'),
  });
  const local = await waitForModels(page, messages, models =>
    models.robots.live === 1 && models.robots.sourceType === 1 && models.robots.draws === 2 &&
    models.models.draws === 2);
  expect(local.docks[0].models.robots).toMatchObject({
    meshes: 3, visual: 2, collision: 0, missing: 1, bridges: 2,
    source: 'browser_robot.urdf',
  });
  expect(local.docks[0].models.robots.status.join(' '))
    .toContain('1 mesh reference(s) unavailable in the browser sandbox');
  const downloaded = await downloadLayoutFromFileMenu(page, screen, messages);
  const xml = downloaded.bytes.toString('utf8');
  expect(xml).toMatch(
    /<robot_model\b(?=[^>]*\bsource_type="file")(?=[^>]*\bsource_value="browser_robot\.urdf")[^>]*>/,
  );
  expect(xml).not.toContain('map_to_arm');
  expect(xml).not.toContain('not_granted_by_browser');
  expect(xml).not.toContain('pj-upload://');

  const layoutChooser = await openLayoutChooser(page);
  await layoutChooser.setFiles({
    name: 'scene3d-local-robot.pj4.xml',
    mimeType: 'application/xml',
    buffer: downloaded.bytes,
  });
  await expect.poll(
    () => messages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=scene3d-local-robot')) || '',
    { timeout: 15000 },
  ).toContain('scene3d-local-robot.pj4.xml');
  const restored = await waitForModels(page, messages, models =>
    models.robots.live === 1 && models.robots.sourceType === 1 && models.robots.draws === 0);
  expect(restored.docks[0].identity).not.toBe(local.docks[0].identity);
  expect(restored.docks[0].models.robots.source).toBe('browser_robot.urdf');
  expect(restored.docks[0].models.robots.status.join(' '))
    .toContain('Select the local URDF again; browser layouts cannot retain host-file access');
  expect(restored.docks[0].readback.rgbSum).toBeGreaterThan(0);
  expect(errors).toEqual([]);
});
