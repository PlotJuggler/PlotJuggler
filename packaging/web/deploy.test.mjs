// SPDX-License-Identifier: MPL-2.0
// node --test packaging/web/deploy.test.mjs

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { chmodSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';

const SCRIPT = path.join(path.dirname(fileURLToPath(import.meta.url)), 'deploy.mjs');
const INDEX = 'plotjuggler-assets/presets/index.json';

// A package whose manifest declares `paths`; a dry run reads nothing else.
function packageWith(...paths) {
  const directory = mkdtempSync(path.join(tmpdir(), 'pj-deploy-'));
  const files = paths.map(file => ({ path: file, representations: { identity: { path: file } } }));
  writeFileSync(path.join(directory, 'manifest.json'), JSON.stringify({ files }));
  return directory;
}

const deploy = (directory, { dryRun = true, env = {} } = {}) => spawnSync(
  process.execPath, [SCRIPT, '--package', directory, ...(dryRun ? ['--dry-run'] : [])],
  { encoding: 'utf8', env: { ...process.env, ...env } });

test('a dry run checks the sample index before it uploads anything or deploys', () => {
  const run = deploy(packageWith('index.html'));
  assert.equal(run.status, 0, run.stderr);
  const commands = run.stdout.split('\n').filter(line => line.startsWith('npx '));
  assert.match(commands[0], new RegExp(`^npx wrangler@\\S+ r2 object get ${INDEX} --remote --pipe$`));
  assert.match(commands.at(-1), / deploy --env preview --var RELEASE:releases\/\w+$/);
});

test('a package built with --presets still deploys: the Worker answers the index route before the release', () => {
  const run = deploy(packageWith('index.html', 'presets/index.json'));
  assert.equal(run.status, 0, run.stderr);
});

test('a missing sample index stops the deploy before anything is uploaded', { skip: process.platform === 'win32' }, () => {
  const bin = mkdtempSync(path.join(tmpdir(), 'pj-npx-'));
  const log = path.join(bin, 'calls');
  // An npx that only fails when asked for an object it does not have, and records every call.
  writeFileSync(path.join(bin, 'npx'), `#!${process.execPath}\n`
    + `require('node:fs').appendFileSync(${JSON.stringify(log)}, process.argv.slice(2).join(' ') + '\\n');\n`
    + "process.exit(process.argv.includes('get') ? 1 : 0);\n");
  chmodSync(path.join(bin, 'npx'), 0o755);
  writeFileSync(log, '');
  const run = deploy(packageWith('index.html'),
    { dryRun: false, env: { PATH: `${bin}${path.delimiter}${process.env.PATH}` } });
  assert.notEqual(run.status, 0);
  assert.match(run.stderr, /upload_preset_index\.mjs/);
  assert.equal(readFileSync(log, 'utf8').trim().split('\n').length, 1, 'only the check ran');
});
