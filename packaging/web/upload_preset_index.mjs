#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Usage: node packaging/web/upload_preset_index.mjs [--dry-run] [<index.json>]
//
// Publishes the hosted app's sample index (default: resources/wasm-presets/index.json) to the asset
// host's bucket, where worker.mjs serves it at /presets/index.json to preview and production alike
// (README.md, "Sample index"). Refuses, before uploading, an index the app could not use; --dry-run
// prints the wrangler command instead of running it.

import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

import { validatePresetCatalog } from '../../scripts/package_wasm.mjs';
import { CACHE_CONTROL, JSON_CONTENT_TYPE, PRESET_INDEX_PATH } from '../../scripts/wasm_http_contract.mjs';
import { ASSET_BUCKET, wrangler } from './wrangler.mjs';

const REPOSITORY_INDEX = path.join(
  path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'resources', 'wasm-presets', 'index.json');

// The wrangler arguments that publish the index at `indexPath`. Nothing is deployed beside the hosted
// index, so every layout and thumbnail must be an absolute URL, and an https page blocks an http one.
export async function putIndexArguments(indexPath) {
  let index;
  try {
    index = JSON.parse(await readFile(indexPath, 'utf8'));
  } catch (error) {
    throw new Error(`${indexPath} is not readable JSON: ${error.message}`, { cause: error });
  }
  validatePresetCatalog(index, []);
  for (const entry of index.presets) {
    for (const reference of [entry.layout, entry.thumbnail].filter(value => value)) {
      if (new URL(reference).protocol === 'http:') {
        throw new Error(`The preset index.json entry ${JSON.stringify(entry.id)} names ${JSON.stringify(reference)}: `
          + 'app.plotjuggler.io is https, so the browser blocks it as mixed content');
      }
    }
  }
  return ['r2', 'object', 'put', `${ASSET_BUCKET}/${PRESET_INDEX_PATH}`, '--file', indexPath,
    '--content-type', JSON_CONTENT_TYPE, '--cache-control', CACHE_CONTROL.revalidate, '--remote'];
}

async function main() {
  const { values, positionals } = parseArgs({ allowPositionals: true, options: { 'dry-run': { type: 'boolean' } } });
  if (positionals.length > 1) {
    throw new Error('Usage: upload_preset_index.mjs [--dry-run] [<index.json>]');
  }
  const indexPath = path.resolve(positionals[0] ?? REPOSITORY_INDEX);
  wrangler(await putIndexArguments(indexPath), values['dry-run']);
  if (!values['dry-run']) {
    console.log(`uploaded ${indexPath} to ${ASSET_BUCKET}/${PRESET_INDEX_PATH}`);
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => {
    process.stderr.write(`${error.stack || error}\n`);
    process.exitCode = 1;
  });
}
