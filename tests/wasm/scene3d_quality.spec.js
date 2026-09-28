// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const {
  bootScene3D,
  dropScene3DRows,
  loadScene3DFixture,
  waitForScene3DModels,
} = require('./support/scene3d_acceptance');

test('Scene3D quality passes activate and recover independently', async ({ page }) => {
  test.setTimeout(150000);
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

  const screen = await bootScene3D(page, messages);
  await loadScene3DFixture(page, screen, messages, 'foxglove_scene_entities_real.mcap');
  // Sorted rows: /markers, /tf/a, /tf/b, /zz_too_many.
  await dropScene3DRows(page, screen, [232, 212]);

  const active = await waitForScene3DModels(page, messages, models =>
    models.models.draws >= 1 && models.quality.hdr.active &&
    models.quality.ssao.active && models.quality.edl.active &&
    models.quality.shadow.active);
  const baseline = active.docks[0].models;
  expect(baseline.quality.shadow).toMatchObject({ ready: true, fit: true, size: 2048 });
  // Shadow casters: one draw per model call (each fixture model has one
  // submesh, so as many as the colour draws). The fixture's marker layer has
  // every solid family resolvable, and markers cast none.
  const solidFamilies = ['cubes', 'spheres', 'cylinders', 'arrows']
    .filter(family => baseline.markers.families[family] > 0).length;
  expect(solidFamilies).toBe(4);
  expect(baseline.quality.shadow.draws).toBe(baseline.models.draws);
  expect(active.docks[0].readback.rgbSum).toBeGreaterThan(0);

  // Fault injection observes the product renderer and a still-live framebuffer;
  // native and CPU tests own the exact shading math.
  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(1, true));
  const noSsao = await waitForScene3DModels(page, messages, models =>
    models.quality.hdr.active && !models.quality.ssao.active && models.quality.edl.active &&
    models.quality.ssao.error.includes('browser acceptance override'));
  expect(noSsao.docks[0].readback.rgbSum).toBeGreaterThan(0);

  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(1, false));
  const ssaoRecovered = await waitForScene3DModels(page, messages, models =>
    models.quality.ssao.active &&
    models.quality.ssao.generation > baseline.quality.ssao.generation);

  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(2, true));
  await waitForScene3DModels(page, messages, models =>
    models.quality.hdr.active && models.quality.ssao.active && !models.quality.edl.active &&
    models.quality.edl.error.includes('browser acceptance override'));
  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(2, false));
  await waitForScene3DModels(page, messages, models =>
    models.quality.edl.active &&
    models.quality.edl.generation > ssaoRecovered.docks[0].models.quality.edl.generation);

  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(0, true));
  const direct = await waitForScene3DModels(page, messages, models =>
    !models.quality.hdr.active && !models.quality.ssao.active && !models.quality.edl.active &&
    models.quality.hdr.error.includes('browser acceptance override'));
  expect(direct.docks[0].models.models.draws).toBe(baseline.models.draws);
  expect(direct.docks[0].readback.rgbSum).toBeGreaterThan(0);

  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(0, false));
  await waitForScene3DModels(page, messages, models =>
    models.quality.hdr.active && models.quality.ssao.active && models.quality.edl.active &&
    models.quality.hdr.generation > baseline.quality.hdr.generation);

  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(3, true));
  const noShadow = await waitForScene3DModels(page, messages, models =>
    models.quality.hdr.active && !models.quality.shadow.active &&
    models.quality.shadow.error.includes('browser acceptance override'));
  expect(noShadow.docks[0].models.models.draws).toBe(baseline.models.draws);
  expect(noShadow.docks[0].readback.rgbSum).toBeGreaterThan(0);

  await page.evaluate(() => window.pjWasmForceScene3DFallbackProbe(3, false));
  await waitForScene3DModels(page, messages, models =>
    models.quality.shadow.active &&
    models.quality.shadow.generation > baseline.quality.shadow.generation);

  expect(messages.some(message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_FAILED'))).toBe(false);
  expect(messages.some(message => message.includes('QRhiWidget: No QRhi'))).toBe(false);
  expect(errors).toEqual([]);
});
