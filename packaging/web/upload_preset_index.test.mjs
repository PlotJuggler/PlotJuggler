// SPDX-License-Identifier: MPL-2.0
// node --test packaging/web/upload_preset_index.test.mjs

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

import { putIndexArguments } from './upload_preset_index.mjs';

const SCRIPT = path.join(path.dirname(fileURLToPath(import.meta.url)), 'upload_preset_index.mjs');
const DIRECTORY = mkdtempSync(path.join(tmpdir(), 'pj-preset-index-'));
const ENTRY = { id: 'a', title: 'A', layout: 'https://assets.example/a.pj4.xml' };

function indexFile(...presets) {
  const file = path.join(DIRECTORY, `index-${Math.random().toString(36).slice(2)}.json`);
  writeFileSync(file, JSON.stringify({ presets }));
  return file;
}

test("the repository's index is uploadable as the hosted app's", () => {
  const run = spawnSync(process.execPath, [SCRIPT, '--dry-run'], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr);
  assert.match(run.stdout, /^npx wrangler@\S+ r2 object put plotjuggler-assets\/presets\/index\.json --file \S+resources\/wasm-presets\/index\.json --content-type 'application\/json; charset=utf-8' --cache-control no-cache --remote$/m);
});

test('a relative reference is refused in words that name no tree: nothing is deployed beside the index', async () => {
  await assert.rejects(putIndexArguments(indexFile({ ...ENTRY, layout: 'a/layout.pj4.xml' })),
    error => /a\/layout\.pj4\.xml/.test(error.message) && !/tree/.test(error.message));
});

test('an http: reference is refused: app.plotjuggler.io is https and the browser blocks mixed content', async () => {
  const insecure = 'http://assets.example/a.png';
  await assert.rejects(putIndexArguments(indexFile({ ...ENTRY, thumbnail: insecure })), /"a".*mixed content/);
  await assert.rejects(putIndexArguments(indexFile({ ...ENTRY, layout: 'http://assets.example/a.pj4.xml' })),
    /mixed content/);
});

test('an empty or null thumbnail is no thumbnail, not a missing file', async () => {
  for (const thumbnail of ['', null]) {
    const args = await putIndexArguments(indexFile({ ...ENTRY, thumbnail }));
    assert.equal(args[0], 'r2');
  }
});
