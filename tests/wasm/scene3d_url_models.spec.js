// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const { wasmHeap } = require('./support/app');
const { latest, layoutOutcome } = require('./support/console');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { openLayoutChooser } = require('./support/pickers');
const { startSecondOrigin } = require('./support/second_origin');
const {
  bootScene3D: boot,
  dropScene3DRows: dropRows,
  fixture,
  loadScene3DFixture: loadFixture,
  waitForScene3DModels: waitForModels,
} = require('./support/scene3d_acceptance');

const cube = fs.readFileSync(path.resolve(__dirname, '../../pj_scene3D/widgets/tests/fixtures/meshes/cube.stl'));

const xmlAttributes = values => Object.entries(values)
  .map(([name, value]) => `${name}="${value.replace(/&/g, '&amp;').replace(/"/g, '&quot;')}"`)
  .join(' ');

// The app's own saved layout plus one local robot layer in its 3D dock.
function withRobot(layout, robot) {
  const xml = layout.toString('utf8');
  expect(xml).toContain('</scene3d>');
  const layer = '<layer local="true" dataset_id="0" topic_name="" object_type="kRobotDescription" '
    + `display_name="robot" visible="true" order="99"><robot_model ${xmlAttributes(robot)}/></layer>`;
  return Buffer.from(xml.replace('</scene3d>', `${layer}</scene3d>`));
}

async function uploadLayout(page, messages, name, bytes) {
  const chooser = await openLayoutChooser(page);
  await chooser.setFiles({ name, mimeType: 'application/xml', buffer: bytes });
  const outcome = () => layoutOutcome(messages, name);
  await expect.poll(outcome, { timeout: 15000 }).not.toBe('');
  return outcome();
}

// A 3D dock showing a recording's /tf, and the layout the app saves for it.
async function sceneWithLayout(page, messages, recording) {
  const screen = await boot(page, messages);
  await loadFixture(page, screen, messages, recording);
  await dropRows(page, screen, [232]);  // sorted rows: /robot_description, /tf
  await waitForModels(page, messages, (_models, dock) => dock.fixed === 'map');
  return { screen, layout: (await downloadLayoutFromFileMenu(page, screen, messages)).bytes };
}

test('a topic robot_description fetches package meshes through the layout roots', async ({ page }) => {
  test.setTimeout(120000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const assets = await startSecondOrigin({
    '/allowed/pj_fixture_description/meshes/cube.stl': { body: cube },
    '/denied/pj_denied_description/meshes/cube.stl': { body: cube, cors: false },
  });
  try {
    const { screen, layout } = await sceneWithLayout(page, messages, 'ros2_package_robot_description_real.mcap');
    const topic = { source_type: 'topic', source_value: '/robot_description', source_topic_name: '/robot_description' };

    // An uploaded file has no location: a relative root fails the load visibly
    // and leaves the workspace as it was (never resolved against the page).
    const before = await waitForModels(page, messages, models => models.robots.live === 0);
    const relative = await uploadLayout(page, messages, 'relative-roots.pj4.xml', withRobot(layout, {
      ...topic,
      package_url_roots: JSON.stringify({ pj_fixture_description: 'assets/pj_fixture_description/' }),
    }));
    expect(relative).toContain('PJ_WASM_LAYOUT_LOAD_FAILED');
    expect(relative).toContain('reason=resources');
    await page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
    await expect.poll(() => latest(messages, 'PJ_WASM_LAYOUT_DIALOG present=1'))
      .toContain('assets/pj_fixture_description/');
    await page.evaluate(() => window.pjWasmDismissLayoutDialogProbe());
    const unchanged = await waitForModels(page, messages, models => models.robots.live === 0);
    expect(unchanged.docks[0].identity).toBe(before.docks[0].identity);
    expect(assets.requests).toEqual([]);

    const loaded = await uploadLayout(page, messages, 'package-roots.pj4.xml', withRobot(layout, {
      ...topic,
      package_url_roots: JSON.stringify({
        pj_fixture_description: `${assets.origin}/allowed/pj_fixture_description/`,
        pj_denied_description: `${assets.origin}/denied/pj_denied_description/`,
      }),
    }));
    expect(loaded).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    // One mesh lands; the missing one (404) and the CORS-denied one fail
    // visibly as placeholders.
    const state = await waitForModels(page, messages, models =>
      models.robots.live === 1 && models.robots.status.join(' ').includes('2 mesh load(s) failed'));
    expect(state.docks[0].models.robots).toMatchObject({
      sourceType: 0, draws: 3, visual: 3, placeholders: 2, meshes: 5, bridges: 2,
    });
    expect(assets.requests).toEqual(expect.arrayContaining([
      'GET /allowed/pj_fixture_description/meshes/cube.stl',
      'GET /allowed/pj_fixture_description/meshes/missing.stl',
      'GET /denied/pj_denied_description/meshes/cube.stl',
    ]));

    // The roots belong to the robot, so the app's own save carries them.
    const saved = (await downloadLayoutFromFileMenu(page, screen, messages)).bytes.toString('utf8');
    expect(saved).toContain(`${assets.origin}/allowed/pj_fixture_description/`);
    expect(errors).toEqual([]);
  } finally {
    await assets.close();
  }
});

test('a URL robot anchors relative meshes and roots at its URL, and a replaced fetch never lands', async ({ page }) => {
  test.setTimeout(120000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const slowRobot = '<robot name="slow_robot"><link name="map"><visual><geometry><box size="1 1 1"/></geometry>'
    + '</visual></link></robot>';
  const assets = await startSecondOrigin({
    '/slow/robot.urdf': { body: slowRobot, hold: true },
    '/robots/v2/url_robot.urdf': { body: fixture('url_robot.urdf') },
    '/robots/v2/meshes/cube.stl': { body: cube },
    '/robots/assets/pj_fixture_description/meshes/cube.stl': { body: cube },
  });
  try {
    const { layout } = await sceneWithLayout(page, messages, 'ros2_robot_description_real.mcap');

    const slow = await uploadLayout(page, messages, 'slow-robot.pj4.xml', withRobot(layout, {
      source_type: 'url', source_value: `${assets.origin}/slow/robot.urdf`,
    }));
    expect(slow).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await expect.poll(() => assets.requests).toContain('GET /slow/robot.urdf');

    // Replaced while the first URDF is still in flight. (No redirect here: the
    // browser's Qt network backend reports the requested URL, not the final one.)
    const source = `${assets.origin}/robots/v2/url_robot.urdf`;
    const replaced = await uploadLayout(page, messages, 'url-robot.pj4.xml', withRobot(layout, {
      source_type: 'url',
      source_value: source,
      package_url_roots: JSON.stringify({ pj_fixture_description: '../assets/pj_fixture_description/' }),
    }));
    expect(replaced).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    const state = await waitForModels(page, messages, models =>
      models.robots.live === 1 && models.robots.draws === 2 && models.robots.placeholders === 0);
    expect(state.docks[0].models.robots).toMatchObject({ sourceType: 2, source, meshes: 6, visual: 2, bridges: 1 });
    expect(assets.requests).toEqual(expect.arrayContaining([
      'GET /robots/v2/meshes/cube.stl',
      'GET /robots/assets/pj_fixture_description/meshes/cube.stl',
    ]));

    assets.release();
    await page.waitForTimeout(1500);
    const settled = await waitForModels(page, messages, models => models.robots.live === 1);
    expect(settled.docks[0].models.robots).toMatchObject({ source, draws: 2, placeholders: 0 });
    expect(errors).toEqual([]);
  } finally {
    await assets.close();
  }
});

// A fetched mesh leaves no copy in the wasm heap: its bytes are dropped once
// parsed. This one does not parse (zeros, not an STL), so the robot keeps a
// placeholder and nothing it built muddies the figure. The bound sits at half a
// copy of the mesh, clear of the ~6 MiB that replacing the layout itself leaves.
test('a URL robot mesh leaves no copy in the wasm heap', async ({ page }) => {
  test.setTimeout(120000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const meshBytes = 30 * 1024 * 1024;  // under the browser's 32 MiB model budget
  const assets = await startSecondOrigin({
    '/robots/v2/url_robot.urdf': { body: fixture('url_robot.urdf') },
    '/robots/v2/meshes/cube.stl': { body: Buffer.alloc(meshBytes) },
    '/robots/assets/pj_fixture_description/meshes/cube.stl': { body: cube },
  });
  try {
    const { layout } = await sceneWithLayout(page, messages, 'ros2_robot_description_real.mcap');
    const before = await wasmHeap(page, messages);
    const loaded = await uploadLayout(page, messages, 'big-mesh-robot.pj4.xml', withRobot(layout, {
      source_type: 'url',
      source_value: `${assets.origin}/robots/v2/url_robot.urdf`,
      package_url_roots: JSON.stringify({ pj_fixture_description: '../assets/pj_fixture_description/' }),
    }));
    expect(loaded).toContain('PJ_WASM_LAYOUT_LOAD_OK');
    await waitForModels(page, messages, models => models.robots.live === 1 && models.robots.placeholders === 1);
    expect(assets.requests).toContain('GET /robots/v2/meshes/cube.stl');

    const after = await wasmHeap(page, messages);
    console.log(`heap in use ${before.inUse} -> ${after.inUse}`);
    expect(after.inUse - before.inUse).toBeLessThan(meshBytes / 2);
    expect(errors).toEqual([]);
  } finally {
    await assets.close();
  }
});
