// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Every FetchContent dependency of the WASM build has a notice in cmake/PjWasmNotices.cmake, and every notice names a
// dependency something declares. A library linked another way (the Qt kit, the Emscripten runtime, 3rdparty/) has a
// section of its own there, which this cannot see.
import assert from 'node:assert/strict';
import { readdir, readFile } from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
// Declared inside a dependency's own build: protobuf fetches Abseil.
const DECLARED_BY_DEPENDENCIES = ['absl'];

async function declaredNames() {
  const cmake = (await readdir(path.join(ROOT, 'cmake'))).filter(name => /^PjWasm.*\.cmake$/.test(name));
  const files = [...cmake.map(name => path.join('cmake', name)), 'CMakeLists.txt', '3rdparty/qwt/CMakeLists.txt'];
  const names = new Set();
  for (const file of files) {
    const text = await readFile(path.join(ROOT, file), 'utf8');
    for (const match of text.matchAll(/FetchContent_Declare\(\s*([A-Za-z0-9_.+-]+)/g)) {
      names.add(match[1].toLowerCase());
    }
  }
  return names;
}

test('every FetchContent dependency of the WASM build has a third-party notice', async () => {
  const declared = await declaredNames();
  const notices = await readFile(path.join(ROOT, 'cmake', 'PjWasmNotices.cmake'), 'utf8');
  const noticed = new Set(
    [...notices.matchAll(/pj_wasm_notice\("\$\{output\}"\s+([A-Za-z0-9_.+-]+)/g)].map(match => match[1].toLowerCase()));
  assert.ok(declared.has('qwt_upstream') && declared.has('pj_wasm_ffmpeg'), 'the scan finds the declarations');
  assert.deepEqual([...declared].filter(name => !noticed.has(name)), [], 'declared without a notice');
  assert.deepEqual(
    [...noticed].filter(name => !declared.has(name) && !DECLARED_BY_DEPENDENCIES.includes(name)), [],
    'a notice for nothing declared');
});
