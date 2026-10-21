// SPDX-License-Identifier: MPL-2.0
// On-demand Python through the product path: a layout carrying Python recipes restores over a loaded dataset, and a
// 3D scene shows what they compute. The browser Transform Editor cannot create one yet, so the recipes come in the
// layout, as a desktop session saves them (DerivedObjects::save). A recipe that does not compile restores silently and
// shows its compile error on its layer once the scene demands it (slice P-c). Without Python in the build the spec
// skips, unless PJ_WASM_PYTHON=1 says the build must have it (WASM CI).
const { test, expect } = require('@playwright/test');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { openLayoutChooser } = require('./support/pickers');
const { bootScene3D, dropScene3DRows, loadScene3DFixture } = require('./support/scene3d_acceptance');
const { requestScene3DFoundationState } = require('./support/scene3d_foundation');

const OWNER = 'layout';
const GOOD_SCRIPT = "def evaluate(inputs, params):\n    return {'cropped': inputs['/points'].filter('x', '>', 0.0)}\n";
const BROKEN_SCRIPT = 'def evaluate(inputs, params:\n    return {}\n';
// The fixture's /points is 300 x 200 points, three of them with a NaN x: 59,997 are drawn. The x > 0 ones are columns
// 0-75 and 226-299 of every row (30,000), two of the NaN points among them (a NaN fails the filter too).
const SOURCE_POINTS = 59997;
const POSITIVE_X_POINTS = 29998;

/// One <on_demand> recipe over the fixture's /points, its output a point cloud named `<owner>/<id>/cropped`.
function onDemandRecipe(id, script) {
  return `<on_demand owner_plugin="${OWNER}" id="${id}" language="python" query_time="1">`
    + '<input name="/points"/><output name="cropped" type="kPointCloud"/>'
    + `<script><![CDATA[${script}]]></script></on_demand>`;
}

/// The browser's own layout with the recipes added and its /points layer replaced by one layer per recipe output.
function withPythonRecipes(xml) {
  const recipes = onDemandRecipe('positive_x', GOOD_SCRIPT) + onDemandRecipe('broken', BROKEN_SCRIPT);
  // QDom writes an empty block self-closed.
  const processors = /<data_processors\s*\/>|<data_processors>/;
  expect(xml, 'browser layout has no <data_processors> block').toMatch(processors);
  let edited = xml.replace(processors, opening =>
    (opening.endsWith('/>') ? `<data_processors>${recipes}</data_processors>` : `<data_processors>${recipes}`));
  const layer = /<layer\b[^>]*topic_name="\/points"[^>]*?(?:\/>|>[\s\S]*?<\/layer>)/;
  expect(edited, 'browser layout has no /points layer').toMatch(layer);
  const original = edited.match(layer)[0];
  const forTopic = (id, order) => original
    .replace('topic_name="/points"', `topic_name="${OWNER}/${id}/cropped"`)
    .replace(/order="\d+"/, `order="${order}"`);
  edited = edited.replace(layer, forTopic('positive_x', 0) + forTopic('broken', 1));
  return edited;
}

const reportCount = messages => messages.filter(message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY')).length;

async function waitForPoints(page, messages, predicate) {
  let state;
  await expect.poll(async () => {
    state = await requestScene3DFoundationState(page, messages, reportCount(messages));
    return state.docks.length === 1 && predicate(state.docks[0].data, state.docks[0]);
  }, { timeout: 60000 }).toBe(true);
  return state.docks[0];
}

test('a layout restores on-demand Python recipes: a scene shows the output and a broken one its error', async ({
  page,
}) => {
  test.setTimeout(240000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const screen = await bootScene3D(page, messages);
  const hasPython = await page.evaluate(() => window.pjWasmPythonAvailableProbe());
  test.skip(!hasPython && process.env.PJ_WASM_PYTHON !== '1', 'this build has no Python (PJ_WASM_WITH_PYTHON off)');
  expect(hasPython, 'PJ_WASM_PYTHON=1, but the build has no Python').toBe(true);

  await loadScene3DFixture(page, screen, messages, 'ros2_scene3d_order_real.mcap');
  // Sorted rows: point, poses, TF, occupancy, second point.
  await dropScene3DRows(page, screen, [212]);
  const source = await waitForPoints(page, messages, data =>
    data.points.live === 1 && data.points.rendered === 1 && data.points.vertices > 0);
  expect(source.data.points.vertices).toBe(SOURCE_POINTS);

  const downloaded = await downloadLayoutFromFileMenu(page, screen, messages);
  const xml = withPythonRecipes(downloaded.bytes.toString('utf8'));
  const chooser = await openLayoutChooser(page);
  await chooser.setFiles({ name: 'python-recipes.pj4.xml', mimeType: 'application/xml', buffer: Buffer.from(xml) });
  await expect.poll(
    () => messages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=python-recipes')) || '',
    { timeout: 30000 },
  ).toContain('python-recipes.pj4.xml').catch((error) => {
    const diagnostics = messages.filter(message => /python|layout/i.test(message)).slice(-6).join('\n');
    throw new Error(`${error.message}\nThe layout's last diagnostics:\n${diagnostics}`);
  });

  // The restored dock (a new identity), with both derived layers: the good recipe's points with x > 0, and the broken
  // recipe's compile error as its layer's warning, demanded by the scene.
  const restored = await waitForPoints(page, messages, (data, dock) =>
    dock.identity !== source.identity && data.points.live === 2 && data.points.rendered === 1
    && data.points.vertices > 0 && data.warnings.some(warning => warning.startsWith('error: ')));
  console.log(`PJ_WASM_PY_LAYOUT source_vertices=${source.data.points.vertices} `
    + `derived_vertices=${restored.data.points.vertices} warnings=${JSON.stringify(restored.data.warnings)}`);
  expect(restored.data.points.vertices).toBe(POSITIVE_X_POINTS);
  expect(restored.data.warnings).toHaveLength(1);
  expect(restored.data.warnings[0]).toMatch(/^error: .*SyntaxError/);
  expect(messages.filter(message => message.includes('PJ_WASM_LAYOUT_LOAD_FAILED'))).toEqual([]);
  expect(errors).toEqual([]);
});
