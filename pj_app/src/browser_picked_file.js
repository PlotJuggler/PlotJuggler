// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Page side of BrowserPickedFile.cpp (linked with --js-library). A picked File is never copied: its staged path
// becomes a read-only MEMFS node whose reads are served from the File itself.
//
// Every pthread's file syscalls are proxied to the browser main thread, which cannot await. So a read posts the File
// slice to a dedicated reader Worker, which reads it with FileReaderSync into a SharedArrayBuffer, while the main
// thread spins on an Atomics flag. The Workers are started, and their first reply awaited, before any pick is
// reported: a Worker created while the main thread spins would never start.
//
// The SHA-256 the layout fingerprint needs runs in a second Worker, so hashing never queues behind or blocks reads.

addToLibrary({
  $PJPick__deps: ['$FS', '$MEMFS', '$mmapAlloc', '$ERRNO_CODES', '$stringToNewUTF8'],
  $PJPick: {
    // Largest slice one reader round trip moves, and the size of the shared scratch buffer.
    kSlice: 8 << 20,
    // Reads shorter than this fetch a block this long and keep it, so small sequential reads cost one round trip.
    kBlock: 1 << 20,
    // A reader that has not answered by then has died; the read fails with EIO instead of spinning forever.
    kTimeoutMs: 30000,
    // Picked Files by pick id, until mounted or forgotten.
    files: new Map(),
    // Pending hashes by request id.
    hashes: new Map(),
    ready: null,
    reader: null,
    hasher: null,
    ctl: null,
    scratch: null,

    // Reader Worker body (stringified; must not reference anything outside itself).
    readerMain: function () {
      const reader = new FileReaderSync();
      let ctl = null;
      let scratch = null;
      self.onmessage = (event) => {
        const message = event.data;
        if (message.ctl) {
          ctl = message.ctl;
          scratch = message.scratch;
          self.postMessage('ready');
          return;
        }
        let got;
        try {
          const bytes = new Uint8Array(reader.readAsArrayBuffer(message));
          new Uint8Array(scratch, 0, bytes.length).set(bytes);
          got = bytes.length;
        } catch (error) {
          got = -1;
        }
        Atomics.store(ctl, 1, got);
        Atomics.store(ctl, 0, 1);
      };
    },

    // Incremental SHA-256 (FIPS 180-4). Stringified into the hasher Worker; must not reference anything outside itself.
    sha256: function () {
      // Int32Array keeps every word a small integer for the JIT; a Uint32Array would box words >= 2^31 as doubles.
      const K = new Int32Array([
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
      ]);
      const H = new Int32Array([
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
      ]);
      const W = new Int32Array(64);
      const pending = new Uint8Array(64);
      let pendingLength = 0;
      let total = 0;

      // Compresses the whole 64-byte blocks of bytes[begin, end).
      const compress = (bytes, begin, end) => {
        let h0 = H[0], h1 = H[1], h2 = H[2], h3 = H[3], h4 = H[4], h5 = H[5], h6 = H[6], h7 = H[7];
        for (let block = begin; block + 64 <= end; block += 64) {
          for (let i = 0; i < 16; i++) {
            const j = block + i * 4;
            W[i] = (bytes[j] << 24) | (bytes[j + 1] << 16) | (bytes[j + 2] << 8) | bytes[j + 3];
          }
          for (let i = 16; i < 64; i++) {
            const w15 = W[i - 15];
            const w2 = W[i - 2];
            const s0 = ((w15 >>> 7) | (w15 << 25)) ^ ((w15 >>> 18) | (w15 << 14)) ^ (w15 >>> 3);
            const s1 = ((w2 >>> 17) | (w2 << 15)) ^ ((w2 >>> 19) | (w2 << 13)) ^ (w2 >>> 10);
            W[i] = (W[i - 16] + s0 + W[i - 7] + s1) | 0;
          }
          let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
          for (let i = 0; i < 64; i++) {
            const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
            const t1 = (h + S1 + ((e & f) ^ (~e & g)) + K[i] + W[i]) | 0;
            const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
            const t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
            h = g;
            g = f;
            f = e;
            e = (d + t1) | 0;
            d = c;
            c = b;
            b = a;
            a = (t1 + t2) | 0;
          }
          h0 = (h0 + a) | 0;
          h1 = (h1 + b) | 0;
          h2 = (h2 + c) | 0;
          h3 = (h3 + d) | 0;
          h4 = (h4 + e) | 0;
          h5 = (h5 + f) | 0;
          h6 = (h6 + g) | 0;
          h7 = (h7 + h) | 0;
        }
        H[0] = h0; H[1] = h1; H[2] = h2; H[3] = h3; H[4] = h4; H[5] = h5; H[6] = h6; H[7] = h7;
      };

      return {
        update: (bytes) => {
          total += bytes.length;
          let offset = 0;
          if (pendingLength > 0) {
            const take = Math.min(64 - pendingLength, bytes.length);
            pending.set(bytes.subarray(0, take), pendingLength);
            pendingLength += take;
            offset = take;
            if (pendingLength < 64) {
              return;
            }
            compress(pending, 0, 64);
            pendingLength = 0;
          }
          const whole = offset + ((bytes.length - offset) & ~63);
          compress(bytes, offset, whole);
          pending.set(bytes.subarray(whole), 0);
          pendingLength = bytes.length - whole;
        },
        hex: () => {
          const tail = new Uint8Array(pendingLength + 9 <= 64 ? 64 : 128);
          tail.set(pending.subarray(0, pendingLength), 0);
          tail[pendingLength] = 0x80;
          const view = new DataView(tail.buffer);
          view.setUint32(tail.length - 8, Math.floor(total / 0x20000000));
          view.setUint32(tail.length - 4, (total * 8) % 0x100000000);
          compress(tail, 0, tail.length);
          let out = '';
          for (let i = 0; i < 8; i++) {
            out += (H[i] >>> 0).toString(16).padStart(8, '0');
          }
          return out;
        },
      };
    },

    // Hasher Worker body: hashes a whole File, slice by slice; '' when it cannot be read whole.
    hasherMain: function (makeSha256, kSlice) {
      const reader = new FileReaderSync();
      self.onmessage = (event) => {
        const message = event.data;
        if (message.ping) {
          self.postMessage({ready: true});
          return;
        }
        let hex;
        try {
          const sha = makeSha256();
          const file = message.file;
          for (let position = 0; position < file.size; position += kSlice) {
            const end = Math.min(file.size, position + kSlice);
            const bytes = new Uint8Array(reader.readAsArrayBuffer(file.slice(position, end)));
            if (bytes.length !== end - position) {
              throw new Error('short read');
            }
            sha.update(bytes);
          }
          hex = sha.hex();
        } catch (error) {
          hex = '';
        }
        self.postMessage({id: message.id, hex: hex});
      };
    },

    workerFrom: function (source) {
      return new Worker(URL.createObjectURL(new Blob([source], {type: 'text/javascript'})));
    },

    // Retires both Workers after one failed (it errored, or a read timed out and may still answer late into the
    // scratch): later reads fail with EIO at once, pending hashes report failure, and the next pick installs anew.
    fail: function () {
      PJPick.reader?.terminate();
      PJPick.hasher?.terminate();
      PJPick.reader = null;
      PJPick.hasher = null;
      PJPick.ready = null;
      const pending = [...PJPick.hashes.values()];
      PJPick.hashes.clear();
      setTimeout(() => pending.forEach((done) => done('')), 0);
    },

    install: function () {
      if (PJPick.ready) {
        return PJPick.ready;
      }
      PJPick.ctl = new Int32Array(new SharedArrayBuffer(8));
      PJPick.scratch = new SharedArrayBuffer(PJPick.kSlice);
      PJPick.reader = PJPick.workerFrom('(' + PJPick.readerMain.toString() + ')();');
      PJPick.hasher = PJPick.workerFrom(
          '(' + PJPick.hasherMain.toString() + ')(' + PJPick.sha256.toString() + ', ' + PJPick.kSlice + ');');
      // A Worker that fails still settles its promise: the pick is reported, and its reads fail with EIO.
      const readerUp = new Promise((resolve) => {
        PJPick.reader.addEventListener('message', () => resolve(), {once: true});
        PJPick.reader.addEventListener('error', () => {
          PJPick.fail();
          resolve();
        });
      });
      const hasherUp = new Promise((resolve) => {
        const onMessage = (event) => {
          if (event.data.ready) {
            resolve();
            return;
          }
          const done = PJPick.hashes.get(event.data.id);
          PJPick.hashes.delete(event.data.id);
          if (done) {
            done(event.data.hex);
          }
        };
        PJPick.hasher.addEventListener('message', onMessage);
        PJPick.hasher.addEventListener('error', () => {
          PJPick.fail();
          resolve();
        });
      });
      PJPick.reader.postMessage({ctl: PJPick.ctl, scratch: PJPick.scratch});
      PJPick.hasher.postMessage({ping: true});
      PJPick.ready = Promise.all([readerUp, hasherUp]);
      return PJPick.ready;
    },

    // Synchronously reads file[position, position + length) into target[offset...] (a Uint8Array); returns the bytes
    // read, fewer only at the end of the File. Throws EIO when the reader fails (the File changed or went away) or
    // does not answer.
    readInto: function (file, position, length, target, offset) {
      let done = 0;
      while (done < length) {
        if (!PJPick.reader) {
          throw new FS.ErrnoError(ERRNO_CODES.EIO);
        }
        const want = Math.min(PJPick.kSlice, length - done);
        Atomics.store(PJPick.ctl, 0, 0);
        PJPick.reader.postMessage(file.slice(position + done, position + done + want));
        const start = performance.now();
        while (Atomics.load(PJPick.ctl, 0) === 0) {
          if (performance.now() - start > PJPick.kTimeoutMs) {
            PJPick.fail();
            throw new FS.ErrnoError(ERRNO_CODES.EIO);
          }
        }
        const got = Atomics.load(PJPick.ctl, 1);
        if (got < 0) {
          throw new FS.ErrnoError(ERRNO_CODES.EIO);
        }
        target.set(new Uint8Array(PJPick.scratch, 0, got), offset + done);
        done += got;
        if (got < want) {
          break;
        }
      }
      return done;
    },

    // stream_ops.read for a mounted pick: `buffer` is a heap view, `offset` an index into it. The first failed read
    // of a node is reported to pj_browser_pick_read_failed(path), later, from the event loop: the File is unreadable
    // from then on, and a reader that cannot tell a read error from the end of file (std::istream) would otherwise
    // load a silently truncated file.
    read: function (node, buffer, offset, length, position) {
      try {
        return PJPick.readUnchecked(node, buffer, offset, length, position);
      } catch (error) {
        if (!node.pjReadFailed) {
          node.pjReadFailed = true;
          const path = FS.getPath(node);
          setTimeout(() => Module._pj_browser_pick_read_failed(stringToNewUTF8(path)), 0);
        }
        throw error;
      }
    },

    readUnchecked: function (node, buffer, offset, length, position) {
      const size = node.usedBytes;
      if (position >= size || length <= 0) {
        return 0;
      }
      const count = Math.min(length, size - position);
      const target = new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.length);
      const cache = node.pjCache;
      if (cache && position >= cache.position && position + count <= cache.position + cache.bytes.length) {
        target.set(cache.bytes.subarray(position - cache.position, position - cache.position + count), offset);
        return count;
      }
      if (count >= PJPick.kBlock) {
        return PJPick.readInto(node.pjFile, position, count, target, offset);
      }
      const block = new Uint8Array(Math.min(PJPick.kBlock, size - position));
      const got = PJPick.readInto(node.pjFile, position, block.length, block, 0);
      node.pjCache = {position: position, bytes: block.subarray(0, got)};
      const served = Math.min(count, got);
      target.set(block.subarray(0, served), offset);
      return served;
    },

    streamOps: null,

    // Makes `path` (absent; its directory exists) a read-only file served from the File of pick `pickId`.
    mount: function (pickId, path) {
      const file = PJPick.files.get(pickId);
      if (!file) {
        return false;
      }
      PJPick.files.delete(pickId);
      PJPick.streamOps ??= {
        llseek: MEMFS.stream_ops.llseek,
        read: (stream, buffer, offset, length, position) => PJPick.read(stream.node, buffer, offset, length, position),
        write: () => {
          throw new FS.ErrnoError(ERRNO_CODES.EPERM);
        },
        // A private copy of the range: callers get the bytes, never a view of the File.
        mmap: (stream, length, position, prot, flags) => {
          const ptr = mmapAlloc(length);
          if (!ptr) {
            throw new FS.ErrnoError(ERRNO_CODES.ENOMEM);
          }
          PJPick.read(stream.node, HEAP8, ptr, length, position);
          return {ptr: ptr, allocated: true};
        },
        msync: () => 0,
      };
      FS.writeFile(path, new Uint8Array(0));
      const node = FS.lookupPath(path).node;
      node.contents = null;
      node.usedBytes = file.size;
      node.pjFile = file;
      node.pjCache = null;
      node.stream_ops = PJPick.streamOps;
      node.mode = (node.mode & ~0o777) | 0o444;
      return true;
    },
  },

  // Shows the browser's file picker through a detached <input type=file>. The chosen File stays page-side under `id`;
  // pj_browser_pick_done(id, name) reports it once the Workers run, with an empty name on cancel. A browser that never
  // fires `cancel` leaves the pick pending, as Qt's own picker does. Must run inside a user gesture.
  pj_browser_pick_file__deps: ['$PJPick', '$stringToNewUTF8'],
  pj_browser_pick_file: function (id, accept_utf8) {
    PJPick.install();
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = UTF8ToString(accept_utf8);
    input.style.display = 'none';
    let settled = false;
    const settle = (file) => {
      if (settled) {
        return;
      }
      settled = true;
      input.remove();
      if (file) {
        PJPick.files.set(id, file);
      }
      PJPick.ready.then(() => Module._pj_browser_pick_done(id, stringToNewUTF8(file ? file.name : '')));
    };
    input.addEventListener('change', () => settle(input.files && input.files.length > 0 ? input.files[0] : null));
    input.addEventListener('cancel', () => settle(null));
    document.body.appendChild(input);
    input.click();
  },

  pj_browser_pick_mount__deps: ['$PJPick'],
  pj_browser_pick_mount: function (pick_id, path_utf8) {
    return PJPick.mount(pick_id, UTF8ToString(path_utf8)) ? 1 : 0;
  },

  pj_browser_pick_forget__deps: ['$PJPick'],
  pj_browser_pick_forget: function (pick_id) {
    PJPick.files.delete(pick_id);
  },

  // Hashes the File behind the mounted pick at `path`; pj_browser_pick_hash_done(request_id, hex) reports it, with an
  // empty hex when the File cannot be read whole (changed or removed since it was picked).
  pj_browser_pick_hash__deps: ['$PJPick', '$stringToNewUTF8'],
  pj_browser_pick_hash: function (request_id, path_utf8) {
    let file;
    try {
      file = FS.lookupPath(UTF8ToString(path_utf8)).node.pjFile || null;
    } catch (error) {
      file = null;
    }
    const done = (hex) => Module._pj_browser_pick_hash_done(request_id, stringToNewUTF8(hex));
    if (!file || !PJPick.hasher) {
      setTimeout(() => done(''), 0);
      return;
    }
    PJPick.hashes.set(request_id, done);
    PJPick.hasher.postMessage({id: request_id, file: file});
  },
});
