#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Usage: node scripts/wasm_size_budget.mjs <manifest.json> <budget bytes> [<side-module budget bytes>]
//
// What a browser downloads to start the app from a package (a scripts/package_wasm.mjs manifest): every file's Brotli
// representation, or its identity when it has none. The sample presets (presets/) and the side modules (the bundle's
// plugins/) are fetched on demand and left out; each side module is reported, and checked against the optional
// side-module budget. Fails when one exceeds its budget.
import { readFile } from 'node:fs/promises';

const [manifestPath, budgetText, sideModuleBudgetText] = process.argv.slice(2);
const budget = Number(budgetText);
const sideModuleBudget = sideModuleBudgetText === undefined ? undefined : Number(sideModuleBudgetText);
if (!manifestPath || !Number.isFinite(budget) || (sideModuleBudget !== undefined && !Number.isFinite(sideModuleBudget))) {
  process.stderr.write('Usage: node scripts/wasm_size_budget.mjs <manifest.json> <budget bytes> [<side-module budget bytes>]\n');
  process.exit(2);
}
const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
const download = file => (file.representations.br ?? file.representations.identity).bytes;
const sideModule = file => file.path.startsWith(`assets/${manifest.bundleId}/plugins/`);
const bytes = manifest.files
  .filter(file => !file.path.startsWith('presets/') && !sideModule(file))
  .reduce((sum, file) => sum + download(file), 0);
process.stdout.write(`Brotli download: ${bytes} B (budget ${budget} B, ${(100 * bytes / budget).toFixed(1)} % of it)\n`);
let failed = bytes > budget;
if (failed) {
  process.stderr.write(`The package exceeds its size budget by ${bytes - budget} B\n`);
}
// Side modules download on first use, each against its own budget.
for (const file of manifest.files.filter(sideModule)) {
  const name = file.path.split('/').at(-1);
  process.stdout.write(`Side module ${name}: ${download(file)} B Brotli, fetched on first use`);
  if (sideModuleBudget !== undefined) {
    process.stdout.write(` (budget ${sideModuleBudget} B)`);
    if (download(file) > sideModuleBudget) {
      process.stderr.write(`\n${name} exceeds its side-module budget by ${download(file) - sideModuleBudget} B`);
      failed = true;
    }
  }
  process.stdout.write('\n');
}
if (failed) {
  process.exit(1);
}
