// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// The pinned wrangler that deploy.mjs and upload_preset_index.mjs run through npx, logged in to the
// PlotJuggler account. Node only: worker.mjs must not import it, since the Worker bundle has no child_process.

import { execFileSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const WRANGLER = 'wrangler@4.118.0';
// wrangler.toml's ASSET_HOST binding.
export const ASSET_BUCKET = 'plotjuggler-assets';
const HERE = path.dirname(fileURLToPath(import.meta.url));

// Runs wrangler from this folder, where wrangler.toml is; with `dryRun`, prints the command instead.
// `stdout` is the child's stdout stdio: 'ignore' for a call made only for its exit code.
export function wrangler(args, dryRun, stdout = 'inherit') {
  if (dryRun) {
    console.log(`npx ${WRANGLER} ${args.map(arg => (/[^\w@%+=:,./-]/.test(arg) ? `'${arg}'` : arg)).join(' ')}`);
    return;
  }
  execFileSync('npx', ['--yes', WRANGLER, ...args], { cwd: HERE, stdio: ['ignore', stdout, 'inherit'] });
}
