// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const VERSIONS_ENV = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'versions.env');

export function sha256(contents) {
  return createHash('sha256').update(contents).digest('hex');
}

export function representation(pathName, contents, contentEncoding = undefined) {
  const result = {
    path: pathName,
    bytes: contents.length,
    sha256: sha256(contents),
  };
  if (contentEncoding) {
    result.contentEncoding = contentEncoding;
  }
  return result;
}

// The value of `key` in versions.env, the single source of the pinned versions.
export async function readVersion(key) {
  const match = (await readFile(VERSIONS_ENV, 'utf8')).match(new RegExp(`^${key}=(.+)$`, 'm'));
  if (!match || !match[1].trim()) {
    throw new Error(`versions.env does not define ${key}`);
  }
  return match[1].trim();
}
