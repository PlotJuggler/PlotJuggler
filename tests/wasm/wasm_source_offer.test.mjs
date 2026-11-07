// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The LGPL source offer (scripts/wasm_source_offer.mjs) names the sources the browser build compiles: nothing here
// downloads them. The script verifies each download against the hash pinned in it.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { access, mkdtemp, readdir, readFile, rm } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { readVersion } from '../../scripts/wasm_manifest.mjs';
import {
  declaredDependencies, dependencyArchive, planSourceOffer, renderSourceOffer,
} from '../../scripts/wasm_source_offer.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = file => readFile(path.join(ROOT, file), 'utf8');

test('FFmpeg is the tarball cmake/PjWasmFfmpeg.cmake compiles', async () => {
  const cmake = await read('cmake/PjWasmFfmpeg.cmake');
  const ffmpeg = (await planSourceOffer()).archives.find(archive => archive.name.startsWith('ffmpeg-'));
  assert.ok(ffmpeg, 'the offer includes FFmpeg');
  assert.ok(cmake.includes(`URL ${ffmpeg.url}\n`), 'same URL');
  assert.ok(cmake.includes(`URL_HASH SHA256=${ffmpeg.sha256}\n`), 'same hash');
});

test('the Qt sources are the PJ_QT_VERSION modules the notices attribute, each with a pinned hash', async () => {
  const version = await readVersion('PJ_QT_VERSION');
  const modules = (await read('cmake/PjWasmNotices.cmake')).match(/foreach\(module IN ITEMS ([^)]+)\)/)[1].split(' ');
  const qt = (await planSourceOffer()).archives.filter(archive => modules.includes(archive.dependency));
  assert.deepEqual(
    qt.map(archive => archive.name), modules.map(module => `${module}-everywhere-src-${version}.tar.xz`));
  const series = version.split('.').slice(0, 2).join('.');
  for (const archive of qt) {
    assert.equal(
      archive.url, `https://download.qt.io/archive/qt/${series}/${version}/submodules/${archive.name}`);
    assert.match(archive.sha256, /^[0-9a-f]{64}$/);
  }
});

test('every dependency the build fetches has exactly one offer entry, with a pin', async () => {
  const { archives } = await planSourceOffer();
  for (const { name } of await declaredDependencies()) {
    assert.equal(archives.filter(archive => archive.dependency === name).length, 1, `${name} has one entry`);
  }
  // Abseil is fetched by protobuf's own build; CPython by PjWasmPython.cmake, outside FetchContent.
  for (const dependency of ['absl', 'cpython']) {
    const entries = archives.filter(archive => archive.dependency === dependency);
    assert.equal(entries.length, 1, `${dependency} has one entry`);
  }
  assert.equal(new Set(archives.map(archive => archive.name)).size, archives.length, 'the file names are unique');
  for (const archive of archives) {
    const pinned = archive.sha256 && /^[0-9a-f]{64}$/.test(archive.sha256) && archive.url.startsWith('https://');
    const gitPin = archive.repository?.startsWith('https://github.com/') && (archive.tag || archive.pinnedBy);
    assert.ok(pinned || gitPin, `${archive.name} is a hashed URL or a git pin`);
  }
});

test('a dependency without a SHA-256 or a tag fails closed', () => {
  const unhashed = { name: 'x', file: 'f.cmake', url: 'https://e.org/x-1.tar.gz' };
  assert.throws(() => dependencyArchive(unhashed), /no URL_HASH/);
  assert.throws(() => dependencyArchive({ name: 'x', file: 'f.cmake' }), /no URL or GIT_TAG/);
});

test('CPython is the tarball PjWasmPython.cmake downloads', async () => {
  const cmake = await read('cmake/PjWasmPython.cmake');
  const version = cmake.match(/set\(_pj_cpython_version (\S+)\)/)[1];
  const python = (await planSourceOffer()).archives.find(archive => archive.dependency === 'cpython');
  assert.equal(python.name, `Python-${version}.tar.xz`);
  assert.equal(python.url, `https://www.python.org/ftp/python/${version}/Python-${version}.tar.xz`);
  assert.ok(cmake.includes(`set(_pj_cpython_sha256 ${python.sha256})`));
});

test('a Qt version without pinned hashes fails closed', async () => {
  await assert.rejects(planSourceOffer({ qtVersion: '6.99.0' }), /no pinned SHA-256 for Qt 6\.99\.0/);
});

test('the Qt Advanced Docking System source is the submodule commit the build compiles', async () => {
  const { ads } = await planSourceOffer();
  const pinned = spawnSync('git', ['ls-tree', 'HEAD', '3rdparty/Qt-Advanced-Docking'], { cwd: ROOT, encoding: 'utf8' });
  assert.equal(ads.commit, pinned.stdout.split(/\s+/)[2]);
  assert.match(ads.commit, /^[0-9a-f]{40}$/);
});

test("PlotJuggler 4's own source is the archive of the revision the package is built from", async () => {
  const { pj4 } = await planSourceOffer();
  const head = spawnSync('git', ['rev-parse', 'HEAD'], { cwd: ROOT, encoding: 'utf8' }).stdout.trim();
  assert.equal(pj4.commit, head);
  assert.equal(pj4.name, `PlotJuggler-4-${head.slice(0, 12)}.tar.gz`);
});

test('SOURCE-OFFER.txt names each archive with its hash, and what rebuilds the module', async () => {
  const unresolved = await planSourceOffer();
  // What writing the offer records for a git pin: the commit its tag names, and the hash of that commit's tarball.
  const plan = {
    ...unresolved,
    archives: unresolved.archives.map((archive, index) => (archive.sha256 ? archive : {
      ...archive,
      commit: `${index}`.padStart(40, 'c'),
      sha256: `${index}`.padStart(64, 'f'),
      url: `https://c.example/${index}`,
    })),
  };
  const text = renderSourceOffer(plan);
  for (const archive of [plan.pj4, ...plan.archives, plan.ads]) {
    assert.ok(text.includes(archive.name) && text.includes(archive.sha256 ?? archive.commit), archive.name);
  }
  for (const archive of plan.archives.filter(entry => entry.repository)) {
    assert.ok(text.includes(archive.commit), `${archive.name} names its commit`);
  }
  for (const input of ['versions.env', 'wasm-ci.yml', 'package_wasm.mjs', 'github.com/PlotJuggler/PJ4/issues',
    'conveyed under the GNU General Public License version 3', 'the desktop application remains MPL-2.0']) {
    assert.ok(text.replace(/\s+/g, ' ').includes(input), `names ${input}`);
  }
});

test('with --links, SOURCE-OFFER.txt gives each library its public address; only PlotJuggler 4 is here', async () => {
  const unresolved = await planSourceOffer();
  // What --links records for a git pin: the commit its tag names and GitHub's tarball of it, with no hash.
  const plan = {
    ...unresolved,
    archives: unresolved.archives.map((archive, index) => (archive.sha256 ? archive : {
      ...archive, commit: `${index}`.padStart(40, 'c'), url: `https://c.example/${index}`,
    })),
  };
  const text = renderSourceOffer(plan, { links: true });
  for (const archive of plan.archives) {
    assert.ok(text.includes(archive.url), `${archive.name} has its address`);
    assert.ok(text.includes(archive.sha256 ?? archive.commit), `${archive.name} has its hash or commit`);
  }
  assert.ok(text.includes(plan.pj4.name) && text.includes(plan.pj4.commit), 'PlotJuggler 4 is the archive here');
  assert.ok(text.includes(`codeload.github.com/PlotJuggler/Qt-Advanced-Docking-System/tar.gz/${plan.ads.commit}`),
    'the docking system is its fork commit on GitHub');
  assert.ok(!text.includes('This directory holds the complete corresponding source'), 'claims no copies');
  assert.ok(!text.replace(/\s+/g, ' ').includes('with the SHA-256 it had then'), 'claims no tarball hashes');
});

test('--dry-run --links lists only the PlotJuggler 4 archive and the offer', async t => {
  const output = await mkdtemp(path.join(os.tmpdir(), 'plotjuggler-source-offer-'));
  t.after(() => rm(output, { recursive: true, force: true }));
  const run = spawnSync('node', ['scripts/wasm_source_offer.mjs', '--links', '--dry-run', '--output', output], {
    cwd: ROOT, encoding: 'utf8',
  });
  assert.equal(run.status, 0, run.stderr);
  const plan = await planSourceOffer();
  assert.ok(run.stdout.includes(plan.pj4.name) && run.stdout.includes('SOURCE-OFFER.txt'));
  assert.ok(!run.stdout.includes(plan.ads.name), 'the docking system is linked, not copied');
  assert.deepEqual(await readdir(output), []);
});

test('--dry-run lists the files and writes nothing; the notices point at the offer it writes', async t => {
  const output = await mkdtemp(path.join(os.tmpdir(), 'plotjuggler-source-offer-'));
  t.after(() => rm(output, { recursive: true, force: true }));
  const run = spawnSync('node', ['scripts/wasm_source_offer.mjs', '--dry-run', '--output', output], {
    cwd: ROOT, encoding: 'utf8',
  });
  assert.equal(run.status, 0, run.stderr);
  const plan = await planSourceOffer();
  const names = [plan.pj4.name, ...plan.archives.map(archive => archive.name), plan.ads.name, 'SOURCE-OFFER.txt'];
  for (const name of names) {
    assert.ok(run.stdout.includes(name), `lists ${name}`);
  }
  assert.deepEqual(await readdir(output), []);
  const notices = await read('cmake/PjWasmNotices.cmake');
  assert.ok(notices.includes('sources/SOURCE-OFFER.txt'), 'THIRD_PARTY_NOTICES.txt names the offer');
});

test('the notices state the bundle licence, and carry the GPL text it names', async () => {
  const notices = await read('cmake/PjWasmNotices.cmake');
  assert.ok(notices.includes('conveyed under the GNU General Public License version 3'), 'the bundle licence');
  assert.ok(notices.includes('GPL-3.0-only entry of the Qt section'), 'where its text is');
  await access(path.join(ROOT, 'cmake', 'wasm_notices', 'GPL-3.0-only.txt'));
});
