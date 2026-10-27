// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { createHash } from 'node:crypto';
import {
  access,
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rm,
  symlink,
  writeFile,
} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { brotliDecompressSync, gunzipSync } from 'node:zlib';

import { CONTENT_TYPES, packageWasm } from '../../scripts/package_wasm.mjs';

const execFileAsync = promisify(execFile);

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');

async function fixture(
  t,
  javascript = 'var PThread = { unusedWorkers: [] }; const memory = {"shared": true};\n',
) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'plotjuggler-wasm-package-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const source = path.join(root, 'source');
  await mkdir(source);
  await Promise.all([
    writeFile(path.join(source, 'plotjuggler4.js'), javascript),
    writeFile(path.join(source, 'plotjuggler4.wasm'), Buffer.from([0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0])),
    writeFile(path.join(source, 'qtloader.js'), 'export const qtLoad = async () => {};\n'),
    writeFile(path.join(source, 'qtlogo.svg'), '<svg xmlns="http://www.w3.org/2000/svg"/>\n'),
    writeFile(path.join(source, 'THIRD_PARTY_NOTICES.txt'), 'PSF LICENSE AGREEMENT\n'.repeat(100)),
  ]);
  return { root, source };
}

test('accepts minified Release pthread markers', async t => {
  const { root, source } = await fixture(
    t,
    'var PThread={unusedWorkers:[]};'
      + 'wasmMemory=new WebAssembly.Memory({initial:4096,maximum:65536,shared:true});\n',
  );
  const manifest = await packageWasm({ source, output: path.join(root, 'release'), version: 'test' });
  assert.equal(manifest.format, 'org.plotjuggler.wasm-deployment.v1');
});

test('still rejects a runtime missing either threaded marker', async t => {
  const missingPthread = await fixture(
    t,
    '// Incidental PThread mention is not an Emscripten pthread runtime.\n'
      + 'wasmMemory=new WebAssembly.Memory({initial:4096,maximum:65536,shared:true});\n',
  );
  await assert.rejects(
    packageWasm({
      source: missingPthread.source,
      output: path.join(missingPthread.root, 'missing-pthread'),
      version: 'test',
    }),
    /is not a threaded Emscripten runtime/,
  );

  const missingSharedMemory = await fixture(
    t,
    'var PThread={unusedWorkers:[]}; const shared=false;\n',
  );
  await assert.rejects(
    packageWasm({
      source: missingSharedMemory.source,
      output: path.join(missingSharedMemory.root, 'missing-shared'),
      version: 'test',
    }),
    /is not a threaded Emscripten runtime/,
  );
});

test('refuses to replace a populated unrelated output directory', async t => {
  const { root, source } = await fixture(t);
  const output = path.join(root, 'unrelated');
  const sentinel = path.join(output, 'keep.txt');
  await mkdir(output);
  await writeFile(sentinel, 'do not delete\n');

  await assert.rejects(
    packageWasm({ source, output, version: 'test' }),
    /Refusing to replace .*non-empty output is not a prior PlotJuggler WASM package/,
  );
  assert.equal(await readFile(sentinel, 'utf8'), 'do not delete\n');
});

test('replaces empty and prior-package outputs but removes stale package files', async t => {
  const { root, source } = await fixture(t);
  const output = path.join(root, 'deploy');
  await mkdir(output);

  const first = await packageWasm({ source, output, version: 'first' });
  assert.equal(first.format, 'org.plotjuggler.wasm-deployment.v1');
  await writeFile(path.join(output, 'stale-package-file'), 'stale\n');

  const second = await packageWasm({ source, output, version: 'second' });
  assert.equal(second.bundleId, first.bundleId);
  const manifest = JSON.parse(await readFile(path.join(output, 'manifest.json'), 'utf8'));
  assert.equal(manifest.version, 'second');
  await assert.rejects(access(path.join(output, 'stale-package-file')), error => error.code === 'ENOENT');
});

test('packages the third-party notices precompressed, and refuses a build without them', async t => {
  const { root, source } = await fixture(t);
  const output = path.join(root, 'with');
  const manifest = await packageWasm({ source, output, version: 'test' });
  const notices = manifest.files.find(file => file.path.endsWith('/THIRD_PARTY_NOTICES.txt'));
  assert.ok(notices, 'the notices are packaged');
  assert.equal(notices.contentType, 'text/plain; charset=utf-8');
  assert.equal(notices.cache, 'immutable');
  assert.ok(notices.representations.br, 'the notices are precompressed');
  assert.equal(
    await readFile(path.join(output, notices.path), 'utf8'),
    'PSF LICENSE AGREEMENT\n'.repeat(100),
  );

  await rm(path.join(source, 'THIRD_PARTY_NOTICES.txt'));
  await assert.rejects(
    packageWasm({ source, output: path.join(root, 'without'), version: 'test' }),
    /Required Qt\/WASM artifact is missing: .*THIRD_PARTY_NOTICES\.txt/,
  );
});

test('serves the notices at a stable root URL, revalidated, beside the immutable copy', async t => {
  const { root, source } = await fixture(t);
  const output = path.join(root, 'stable');
  const manifest = await packageWasm({ source, output, version: 'test' });
  const stable = manifest.files.find(file => file.path === 'THIRD_PARTY_NOTICES.txt');
  assert.ok(stable, 'the notices are at the deployment root');
  assert.equal(stable.cache, 'revalidate');
  assert.ok(stable.representations.br, 'the stable copy is precompressed too');
  const immutable = manifest.files.find(file => file.path.endsWith('/THIRD_PARTY_NOTICES.txt'));
  assert.equal(stable.representations.identity.sha256, immutable.representations.identity.sha256);
  assert.match(await readFile(path.join(output, '_headers'), 'utf8'), /\n\/THIRD_PARTY_NOTICES\.txt\n {2}Cache-Control: no-cache\n/);
});

test("the About dialog's licences link is a file the package serves at its root", async t => {
  const dialog = await readFile(path.join(ROOT, 'pj_app', 'src', 'ui', 'AboutDialog.ui'), 'utf8');
  const label = dialog.match(/<widget class="QLabel" name="noticesLabel">[\s\S]*?<\/widget>/);
  assert.ok(label, 'AboutDialog.ui has a noticesLabel');
  const href = label[0].match(/href="([^"]+)"/)?.[1];
  assert.ok(href, 'the label links somewhere');
  assert.match(label[0], /<bool>true<\/bool>/, 'the link opens in the browser (openExternalLinks)');

  const { root, source } = await fixture(t);
  const manifest = await packageWasm({ source, output: path.join(root, 'link'), version: 'test' });
  assert.ok(manifest.files.some(file => file.path === href), `the package serves ${href} at its root`);
});

test('the entrypoint explains a page that cannot run instead of booting it', async t => {
  const { root, source } = await fixture(t);
  const output = path.join(root, 'deploy');
  await packageWasm({ source, output, version: 'test' });
  const entrypoint = await readFile(path.join(output, 'index.html'), 'utf8');

  // Both conditions are checked before the threaded runtime is loaded.
  assert.match(entrypoint, /isSecureContext[\s\S]*crossOriginIsolated[\s\S]*await qtLoad\(\{/);
  assert.match(entrypoint, /not served over HTTPS/);
  assert.match(entrypoint, /not cross-origin isolated/);
  assert.ok(entrypoint.includes('The app must be served over HTTPS, or from localhost, with the headers '
    + 'Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp.'));
});

test("the loading page is PlotJuggler's own, with no Qt template text", async t => {
  const { root, source } = await fixture(t);
  const output = path.join(root, 'deploy');
  const manifest = await packageWasm({ source, output, version: 'test' });
  const entrypoint = await readFile(path.join(output, 'index.html'), 'utf8');

  const logo = `assets/${manifest.bundleId}/plotjuggler.svg`;
  assert.ok(entrypoint.includes(`<link rel="icon" href="${logo}"`));
  assert.ok(entrypoint.includes('<title>PlotJuggler</title>'));
  assert.doesNotMatch(entrypoint, /qtlogo|Qt for WebAssembly|plotjuggler4</);
  assert.doesNotMatch(entrypoint, /@[A-Z_]+@/, 'every template placeholder is filled');
  assert.deepEqual(
    await readFile(path.join(output, logo)),
    await readFile(path.join(ROOT, 'resources', 'plotjuggler.svg')),
  );
  assert.ok(!manifest.files.some(file => file.path.endsWith('qtlogo.svg')));
});

test('the loading page measures the download against the unpacked module', async t => {
  const { root, source } = await fixture(t);
  const output = path.join(root, 'deploy');
  const manifest = await packageWasm({ source, output, version: 'test' });
  const entrypoint = await readFile(path.join(output, 'index.html'), 'utf8');

  // Browsers count decoded bytes, while Content-Length is the compressed size, so the page carries the real one.
  assert.ok(entrypoint.includes('const WASM_BYTES = 8;'));
  assert.ok(entrypoint.includes(`'assets/${manifest.bundleId}/plotjuggler4.wasm'`));
  // The page compiles the module itself and hands it to Qt's loader, which then never fetches it.
  assert.match(entrypoint, /await qtLoad\(\{[\s\S]*module: wasmModule/);
});

// docs/wasm/Caddyfile serves packages on robots. Caddy cannot read the manifest, so its MIME table
// is a copy of CONTENT_TYPES, compared here both ways: a missing row would fall back to the host's
// own MIME database, which differs between robots.
test('the robot Caddyfile repeats every deployable type and forwards to the stock bridge port', async () => {
  const caddyfile = await readFile(new URL('../../docs/wasm/Caddyfile', import.meta.url), 'utf8');
  const types = new Map();
  for (const [, extension, contentType] of caddyfile.matchAll(/^\s*~\(\?i\)\\(\.\w+)\$ "([^"]+)"$/gm)) {
    types.set(extension, contentType);
  }
  const extensionless = caddyfile.match(/^\s*~\/\[\^\/\.\]\+\$ "([^"]+)"$/m);
  if (extensionless) {
    types.set('', extensionless[1]);
  }
  assert.deepStrictEqual(types, CONTENT_TYPES);
  // pj_bridge's own default port, unless PJ_BRIDGE_UPSTREAM names another.
  assert.match(caddyfile, /reverse_proxy \{\$PJ_BRIDGE_UPSTREAM:127\.0\.0\.1:9090\}/);
});

// A preset tree shaped like resources/wasm-presets: nested folders, data, a
// model package with an extensionless license, and the index beside them. The
// large files are over the size floor below which nothing is precompressed.
const MCAP_MAGIC = Buffer.from([0x89, 0x4d, 0x43, 0x41, 0x50, 0x30, 0x0d, 0x0a]);
const PNG_MAGIC = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const PRESET_FILES = {
  'README.md': '# Samples\n',
  'index.json': JSON.stringify({
    presets: [{ id: 'fr3', title: 'FR3', layout: 'fr3/layout.pj4.xml', thumbnail: 'fr3/thumbnail.png' }],
  }),
  'fr3/layout.pj4.xml': '<root/>\n',
  'fr3/data.mcap': Buffer.concat([MCAP_MAGIC, Buffer.alloc(4096)]),
  'fr3/thumbnail.png': Buffer.concat([PNG_MAGIC, Buffer.alloc(4096)]),
  'fr3/assets/franka_description/LICENSE': 'Apache License\n'.repeat(100),
  'fr3/assets/franka_description/meshes/visual/link0.dae': '<COLLADA/>\n'.repeat(200),
  'fr3/assets/franka_description/meshes/visual/link2.glb': Buffer.concat([Buffer.from('glTF'), Buffer.alloc(4096)]),
  'fr3/assets/franka_description/meshes/collision/link0.stl': 'solid link0\nendsolid link0\n',
  'fr3/assets/franka_description/meshes/collision/link1.STL': `solid link1\n${'facet normal 0 0 1\n'.repeat(100)}endsolid link1\n`,
  'signals/data.csv': 'time,value\n'.repeat(200),
};
const PRESET_CONTENT_TYPES = {
  'README.md': 'text/markdown; charset=utf-8',
  'index.json': 'application/json; charset=utf-8',
  'fr3/layout.pj4.xml': 'application/xml',
  'fr3/data.mcap': 'application/octet-stream',
  'fr3/thumbnail.png': 'image/png',
  'fr3/assets/franka_description/LICENSE': 'text/plain; charset=utf-8',
  'fr3/assets/franka_description/meshes/visual/link0.dae': 'model/vnd.collada+xml',
  'fr3/assets/franka_description/meshes/visual/link2.glb': 'model/gltf-binary',
  'fr3/assets/franka_description/meshes/collision/link0.stl': 'model/stl',
  'fr3/assets/franka_description/meshes/collision/link1.STL': 'model/stl',
  'signals/data.csv': 'text/csv; charset=utf-8',
};
// Text and mesh types of at least 1 KiB gain .br and .gz; already-compressed formats
// (MCAP chunks are zstd, PNG is deflate, GLB commonly embeds both) and small files do not.
const PRECOMPRESSED_PRESETS = [
  'fr3/assets/franka_description/LICENSE',
  'fr3/assets/franka_description/meshes/visual/link0.dae',
  'fr3/assets/franka_description/meshes/collision/link1.STL',
  'signals/data.csv',
];

async function writeTree(root, files) {
  for (const [relative, contents] of Object.entries(files)) {
    const destination = path.join(root, ...relative.split('/'));
    await mkdir(path.dirname(destination), { recursive: true });
    await writeFile(destination, contents);
  }
}

// Every regular file below `root`, as sorted '/'-separated relative paths.
async function listTree(root, prefix = '') {
  const result = [];
  for (const entry of await readdir(path.join(root, prefix), { withFileTypes: true })) {
    const relative = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) {
      result.push(...await listTree(root, relative));
    } else {
      result.push(relative);
    }
  }
  return result.sort();
}

test('preset tree is fully declared', async t => {
  const { root, source } = await fixture(t);
  const presets = path.join(root, 'presets-source');
  await writeTree(presets, PRESET_FILES);
  const output = path.join(root, 'deploy');

  const manifest = await packageWasm({ source, output, version: 'test', presets });

  const declared = new Map(manifest.files.map(file => [file.path, file]));
  for (const [relative, contents] of Object.entries(PRESET_FILES)) {
    const logicalPath = `presets/${relative}`;
    const file = declared.get(logicalPath);
    assert.ok(file, `${logicalPath} is not declared`);
    const bytes = Buffer.from(contents);
    assert.equal(file.contentType, PRESET_CONTENT_TYPES[relative], logicalPath);
    assert.equal(file.cache, 'revalidate', logicalPath);
    assert.deepEqual(
      file.representations.identity,
      { path: logicalPath, bytes: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') },
      logicalPath,
    );
    assert.deepEqual(await readFile(path.join(output, logicalPath)), bytes);
  }
  // Nothing is deployed that the manifest does not declare.
  const representations = manifest.files.flatMap(file => Object.values(file.representations).map(item => item.path));
  assert.deepEqual(
    (await listTree(output)).filter(relative => !['manifest.json', '_headers'].includes(relative)),
    representations.sort(),
  );
  assert.match(await readFile(path.join(output, '_headers'), 'utf8'), /\n\/presets\/\*\n {2}Cache-Control: no-cache\n/);
});

test('compressible preset files are precompressed like the bundle, the rest stay identity-only', async t => {
  const { root, source } = await fixture(t);
  const presets = path.join(root, 'presets-source');
  await writeTree(presets, PRESET_FILES);
  const output = path.join(root, 'deploy');

  const manifest = await packageWasm({ source, output, version: 'test', presets });

  const declared = new Map(manifest.files.map(file => [file.path, file]));
  for (const [relative, contents] of Object.entries(PRESET_FILES)) {
    const logicalPath = `presets/${relative}`;
    const { representations } = declared.get(logicalPath);
    if (!PRECOMPRESSED_PRESETS.includes(relative)) {
      assert.deepEqual(Object.keys(representations), ['identity'], logicalPath);
      continue;
    }
    assert.deepEqual(Object.keys(representations).sort(), ['br', 'gzip', 'identity'], logicalPath);
    const bytes = Buffer.from(contents);
    for (const [name, suffix, decode] of [['br', '.br', brotliDecompressSync], ['gzip', '.gz', gunzipSync]]) {
      const encoded = await readFile(path.join(output, `${logicalPath}${suffix}`));
      assert.equal(representations[name].path, `${logicalPath}${suffix}`);
      assert.equal(representations[name].contentEncoding, name);
      assert.equal(representations[name].bytes, encoded.length);
      assert.equal(representations[name].sha256, createHash('sha256').update(encoded).digest('hex'));
      assert.ok(encoded.length < bytes.length, `${logicalPath} (${name}) is smaller than its identity`);
      assert.deepEqual(decode(encoded), bytes, `${logicalPath} (${name}) decodes to its identity`);
    }
    assert.equal(declared.get(logicalPath).cache, 'revalidate', logicalPath);
  }
});

test('unsafe tree entries are rejected', async t => {
  const { root, source } = await fixture(t);
  await writeFile(path.join(root, 'outside.csv'), 'time,value\n0,1\n');
  const cases = [
    ['a symlink out of the tree', async presets => symlink('../outside.csv', path.join(presets, 'escape.csv'))],
    ['an absolute symlink', async presets => symlink(path.join(root, 'outside.csv'), path.join(presets, 'absolute.csv'))],
    ['a dotfile', async presets => writeFile(path.join(presets, '.DS_Store'), 'x')],
    ['a dot directory', async presets => writeTree(presets, { '.git/config': '[core]\n' })],
    ['a symlinked directory', async presets => symlink(root, path.join(presets, 'linked'))],
    ['an unsafe file name', async presets => writeFile(path.join(presets, 'what?.csv'), 'time\n')],
    ['an undeployable file type', async presets => writeFile(path.join(presets, 'notes.docx'), 'x')],
  ];
  for (const [index, [name, addEntry]] of cases.entries()) {
    const presets = path.join(root, `presets-${index}`);
    await writeTree(presets, { 'index.json': '{"presets": []}\n' });
    await addEntry(presets);
    const output = path.join(root, `deploy-${path.basename(presets)}`);
    await assert.rejects(packageWasm({ source, output, version: 'test', presets }), /preset/i, name);
    await assert.rejects(access(output), error => error.code === 'ENOENT', `${name} wrote an output`);
  }

  // A tree that contains, or is inside, the output would be deleted with it.
  const presets = path.join(root, 'nested');
  await writeTree(presets, { 'index.json': '{"presets": []}\n' });
  await assert.rejects(
    packageWasm({ source, output: path.join(presets, 'deploy'), version: 'test', presets }),
    /distinct, non-nested/,
  );
  await assert.rejects(
    packageWasm({ source, output: root, version: 'test', presets }),
    /distinct, non-nested/,
  );
});

test('a preset index must list only deployed files', async t => {
  const { root, source } = await fixture(t);
  const index = presets => JSON.stringify({ presets });
  const fr3 = { id: 'fr3', title: 'FR3', layout: 'fr3/layout.pj4.xml', thumbnail: 'fr3/thumbnail.png' };
  const tree = { 'fr3/layout.pj4.xml': '<root/>\n', 'fr3/thumbnail.png': Buffer.from([0x89, 0x50]) };
  const cases = [
    ['no index', {}, /index\.json/],
    ['malformed JSON', { 'index.json': '{"presets": [' }, /index\.json/],
    ['no presets array', { 'index.json': '{"presets": {}}' }, /presets array/],
    ['a missing layout', { 'index.json': index([{ ...fr3, layout: 'fr3/missing.pj4.xml' }]) }, /fr3\/missing\.pj4\.xml/],
    ['a missing thumbnail', { 'index.json': index([{ ...fr3, thumbnail: 'gone.png' }]) }, /gone\.png/],
    ['a reference out of the tree', { 'index.json': index([{ ...fr3, layout: '../fr3/layout.pj4.xml' }]) },
      /\.\.\/fr3\/layout\.pj4\.xml/],
    // The browser rejects the whole index for any of these, so packaging must too.
    ['no layout key', { 'index.json': index([fr3, { id: 'broken' }]) }, /"broken" has no layout/],
    ['an empty layout', { 'index.json': index([{ ...fr3, layout: '' }]) }, /"fr3" has no layout/],
    ['no id', { 'index.json': index([{ ...fr3, id: undefined }]) }, /#1 has no id/],
    ['an empty id', { 'index.json': index([{ ...fr3, id: '' }]) }, /#1 has no id/],
    ['a duplicate id', { 'index.json': index([fr3, { ...fr3 }]) }, /duplicate preset id "fr3"/],
    ['a numeric id', { 'index.json': index([{ ...fr3, id: 3 }]) }, /#1 has a field that is not a string/],
    ['an object title', { 'index.json': index([{ ...fr3, title: {} }]) }, /#1 has a field that is not a string/],
    ['a numeric description', { 'index.json': index([{ ...fr3, description: 1 }]) },
      /#1 has a field that is not a string/],
    ['a numeric layout', { 'index.json': index([{ ...fr3, layout: 7 }]) }, /#1 has a field that is not a string/],
    ['a boolean thumbnail', { 'index.json': index([{ ...fr3, thumbnail: true }]) },
      /#1 has a field that is not a string/],
    ['an entry that is not an object', { 'index.json': index([fr3, 'nav']) }, /#2 is not an object/],
    ['a null entry', { 'index.json': index([null]) }, /#1 is not an object/],
  ];
  for (const [index, [name, files, message]] of cases.entries()) {
    const presets = path.join(root, `presets-${index}`);
    await writeTree(presets, { ...tree, ...files });
    await assert.rejects(
      packageWasm({ source, output: path.join(root, `deploy-${name}`), version: 'test', presets }), message, name);
  }

  // Absolute http(s) references point elsewhere and are left to the browser; a null thumbnail is absent.
  const presets = path.join(root, 'presets-remote');
  await writeTree(presets, {
    ...tree,
    'index.json': index([
      { ...fr3, thumbnail: null },
      { id: 'robot', title: 'Robot', layout: 'https://robot.example/presets/layout.pj4.xml' },
    ]),
  });
  const manifest = await packageWasm({ source, output: path.join(root, 'deploy-remote'), version: 'test', presets });
  assert.ok(manifest.files.some(file => file.path === 'presets/index.json'));
});

test('packaging remains deterministic', async t => {
  const { root, source } = await fixture(t);
  const presets = path.join(root, 'presets-source');
  await writeTree(presets, PRESET_FILES);
  const first = path.join(root, 'deploy-a');
  const second = path.join(root, 'deploy-b');

  await packageWasm({ source, output: first, version: 'test', presets });
  // The command line packages the same tree to the same bytes.
  await execFileAsync(process.execPath, [
    fileURLToPath(new URL('../../scripts/package_wasm.mjs', import.meta.url)),
    '--source', source, '--output', second, '--version', 'test', '--presets', presets,
  ]);

  const files = await listTree(first);
  assert.ok(files.includes('presets/fr3/assets/franka_description/LICENSE'));
  assert.ok(files.includes('presets/fr3/assets/franka_description/meshes/visual/link0.dae.br'));
  assert.ok(files.includes('presets/fr3/assets/franka_description/meshes/visual/link0.dae.gz'));
  assert.deepEqual(await listTree(second), files);
  for (const relative of files) {
    assert.deepEqual(await readFile(path.join(second, relative)), await readFile(path.join(first, relative)), relative);
  }
});
