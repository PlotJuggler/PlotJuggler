#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Usage: node scripts/wasm_size_budget.mjs <manifest.json> <budget bytes>
//
// What a browser downloads to start the app from a package (a scripts/package_wasm.mjs manifest): every file's Brotli
// representation, or its identity when it has none. The sample presets (presets/) are fetched on demand and left out.
// Fails when it exceeds the budget.
import { readFile } from 'node:fs/promises';

const [manifestPath, budgetText] = process.argv.slice(2);
const budget = Number(budgetText);
if (!manifestPath || !Number.isFinite(budget)) {
  process.stderr.write('Usage: node scripts/wasm_size_budget.mjs <manifest.json> <budget bytes>\n');
  process.exit(2);
}
const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
const bytes = manifest.files
  .filter(file => !file.path.startsWith('presets/'))
  .reduce((sum, file) => sum + (file.representations.br ?? file.representations.identity).bytes, 0);
process.stdout.write(`Brotli download: ${bytes} B (budget ${budget} B, ${(100 * bytes / budget).toFixed(1)} % of it)\n`);
if (bytes > budget) {
  process.stderr.write(`The package exceeds its size budget by ${bytes - budget} B\n`);
  process.exit(1);
}
