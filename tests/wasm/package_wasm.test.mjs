// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import assert from 'node:assert/strict';
import {
  access,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { packageWasm } from '../../scripts/package_wasm.mjs';

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
    writeFile(
      path.join(source, 'plotjuggler4.html'),
      '<script src="qtloader.js"></script>\n'
        + '<img src="qtlogo.svg">\n'
        + '<script src="plotjuggler4.js"></script>\n'
        + 'const instance = await qtLoad({\n});\n',
    ),
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
