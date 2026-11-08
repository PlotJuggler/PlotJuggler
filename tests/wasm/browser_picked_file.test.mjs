// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The page side of lazy browser picks (pj_app/src/browser_picked_file.js) without a browser: its Worker protocol is
// driven through fake Workers, and its SHA-256 is checked against Node's.
import assert from 'node:assert/strict';
import { createHash, randomBytes } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const EIO = 29;

class ErrnoError extends Error {
  constructor(errno) {
    super(`errno ${errno}`);
    this.errno = errno;
  }
}

// Evaluates the --js-library file the way Emscripten links it: `PJPick` is a global of the library's own scope.
async function loadLibrary() {
  const source = await readFile(path.join(ROOT, 'pj_app', 'src', 'browser_picked_file.js'), 'utf8');
  const body = `let PJPick; const library = {}; const addToLibrary = (o) => Object.assign(library, o);
    ${source}
    PJPick = library.$PJPick; return library;`;
  const library = new Function('FS', 'ERRNO_CODES', 'MEMFS', 'Module', 'mmapAlloc', 'stringToNewUTF8', body)(
      { ErrnoError }, { EIO, EPERM: 63 }, {}, {}, () => 0, (s) => s);
  return library.$PJPick;
}

// A reader Worker that answers each request one request late, as a Worker that missed a deadline would.
class LateReader extends EventTarget {
  constructor(pick) {
    super();
    this.pick = pick;
    this.queue = [];
    this.terminated = false;
  }

  postMessage(blob) {
    this.queue.push(blob);
    if (this.queue.length > 1) {
      const late = this.queue.shift();
      new Uint8Array(this.pick.scratch, 0, late.length).fill(late.fill);
      Atomics.store(this.pick.ctl, 1, late.length);
      Atomics.store(this.pick.ctl, 0, 1);
    }
  }

  terminate() {
    this.terminated = true;
  }
}

class SilentWorker extends EventTarget {
  postMessage() {}
  terminate() {
    this.terminated = true;
  }
}

const fakeFile = { slice: (begin, end) => ({ length: end - begin, fill: begin === 0 ? 0xaa : 0xbb }) };

function readFails(pick, position) {
  const target = new Uint8Array(4);
  assert.throws(() => pick.readInto(fakeFile, position, 4, target, 0), (error) => error.errno === EIO);
  return target;
}

test('a timed-out read never hands its late reply to the next read', async () => {
  const pick = await loadLibrary();
  pick.kTimeoutMs = 20;
  pick.ctl = new Int32Array(new SharedArrayBuffer(8));
  pick.scratch = new SharedArrayBuffer(16);
  const reader = new LateReader(pick);
  pick.reader = reader;
  pick.hasher = new SilentWorker();
  pick.ready = Promise.resolve();
  const hashed = new Promise((resolve) => pick.hashes.set(7, resolve));

  readFails(pick, 0);
  // The reader may still answer the first request: no later read may take that answer as its own.
  assert.deepEqual([...readFails(pick, 4)], [0, 0, 0, 0]);
  assert.equal(reader.terminated, true);
  assert.equal(pick.ready, null, 'the next pick starts new Workers');
  assert.equal(await hashed, '', 'a pending hash reports failure');
});

test('a Worker that fails to start settles the pick and fails reads at once', async () => {
  const pick = await loadLibrary();
  const workers = [];
  pick.workerFrom = () => {
    const worker = new SilentWorker();
    workers.push(worker);
    return worker;
  };
  const ready = pick.install();
  const hashed = new Promise((resolve) => pick.hashes.set(3, resolve));
  workers[0].dispatchEvent(new Event('error'));
  workers[1].dispatchEvent(new MessageEvent('message', { data: { ready: true } }));
  await ready;
  const started = performance.now();
  readFails(pick, 0);
  assert.ok(performance.now() - started < 1000, 'no spin until the timeout');
  assert.equal(await hashed, '');
  assert.ok(workers.every((worker) => worker.terminated));
});

test('the incremental SHA-256 matches Node for every length and split', async () => {
  const pick = await loadLibrary();
  const makeSha256 = pick.sha256;
  const lengths = [0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000, (1 << 20) + 17];
  for (const length of lengths) {
    const bytes = randomBytes(length);
    const expected = createHash('sha256').update(bytes).digest('hex');
    for (const parts of [1, 2, 7]) {
      const hasher = makeSha256();
      let position = 0;
      for (let part = 1; part <= parts; part += 1) {
        const end = part === parts ? length : Math.floor((length * part) / parts);
        hasher.update(new Uint8Array(bytes.buffer, bytes.byteOffset + position, end - position));
        position = end;
      }
      assert.equal(hasher.hex(), expected, `length ${length} in ${parts} parts`);
    }
  }
});
