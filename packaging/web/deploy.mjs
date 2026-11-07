#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Usage: node packaging/web/deploy.mjs --package <deployment> [--env preview|production] [--dry-run]
//
// Publishes a deployment package (scripts/package_wasm.mjs, with its sources/ from
// scripts/wasm_source_offer.mjs) as one release of the hosted browser app: uploads every file the
// manifest declares, the manifest and sources/ to the R2 bucket under releases/<manifest hash>/,
// then deploys the Worker with that prefix as RELEASE. Switching releases is the Worker deploy,
// so a page never mixes two releases; earlier releases stay in the bucket for a rollback (deploy
// again with an older package). Runs the pinned wrangler through npx, logged in to the account.

import { execFileSync } from 'node:child_process';
import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { sha256 } from '../../scripts/wasm_manifest.mjs';

const BUCKET = 'plotjuggler-web';
const WRANGLER = 'wrangler@4.118.0';
const HERE = path.dirname(fileURLToPath(import.meta.url));

function parseArguments(argv) {
  const options = { env: 'preview', dryRun: false };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--dry-run') {
      options.dryRun = true;
    } else if (argument === '--package' || argument === '--env') {
      options[argument.slice(2)] = argv[index + 1];
      index += 1;
    } else {
      throw new Error(`Unknown argument: ${argument}`);
    }
  }
  if (!options.package || !['preview', 'production'].includes(options.env)) {
    throw new Error('Usage: deploy.mjs --package <deployment> [--env preview|production] [--dry-run]');
  }
  return options;
}

function walk(directory, prefix = '') {
  return readdirSync(directory).flatMap((name) => {
    const full = path.join(directory, name);
    const logical = prefix ? `${prefix}/${name}` : name;
    return statSync(full).isDirectory() ? walk(full, logical) : [logical];
  });
}

function wrangler(args, dryRun) {
  if (dryRun) {
    console.log(`wrangler ${args.join(' ')}`);
    return;
  }
  execFileSync('npx', ['--yes', WRANGLER, ...args], { cwd: HERE, stdio: ['ignore', 'inherit', 'inherit'] });
}

const options = parseArguments(process.argv.slice(2));
const root = path.resolve(options.package);
const manifestBytes = readFileSync(path.join(root, 'manifest.json'));
const manifest = JSON.parse(manifestBytes);
const offer = path.join(root, 'sources', 'SOURCE-OFFER.txt');
// The offer is outside the manifest: hashing it too keeps a regenerated offer from replacing a live release's.
const hashed = existsSync(offer) ? [manifestBytes, readFileSync(offer)] : [manifestBytes];
const release = `releases/${sha256(Buffer.concat(hashed)).slice(0, 16)}`;

const files = new Set(['manifest.json']);
for (const file of manifest.files) {
  for (const representation of Object.values(file.representations)) {
    files.add(representation.path);
  }
}
if (existsSync(offer)) {
  for (const file of walk(path.join(root, 'sources'), 'sources')) {
    files.add(file);
  }
} else if (options.env === 'production') {
  throw new Error(`${root}/sources/SOURCE-OFFER.txt is missing: the GPL-3.0 bundle needs its source offer`);
}

console.log(`release ${release}: ${files.size} files`);
for (const file of files) {
  wrangler(['r2', 'object', 'put', `${BUCKET}/${release}/${file}`, '--file', path.join(root, file), '--remote'],
    options.dryRun);
}
// Production is wrangler.toml's top level, which wrangler names with an empty --env.
const environment = options.env === 'preview' ? ['--env', 'preview'] : ['--env='];
wrangler(['deploy', ...environment, '--var', `RELEASE:${release}`], options.dryRun);
console.log(`deployed ${release} (${options.env})`);
