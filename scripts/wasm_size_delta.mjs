#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Usage: node scripts/wasm_size_delta.mjs <base manifest.json> <candidate manifest.json> <budget bytes>
//
// Compares what a browser downloads from two packages (scripts/package_wasm.mjs manifests): every file's Brotli
// representation, or its identity when it has none. Fails when the candidate exceeds the base by more than the budget.
import { readFile } from 'node:fs/promises';

async function brotliBytes(manifestPath) {
  const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
  return manifest.files.reduce(
    (sum, file) => sum + (file.representations.br ?? file.representations.identity).bytes, 0);
}

const [basePath, candidatePath, budgetText] = process.argv.slice(2);
const budget = Number(budgetText);
if (!candidatePath || !Number.isFinite(budget)) {
  process.stderr.write('Usage: node scripts/wasm_size_delta.mjs <base manifest> <candidate manifest> <budget bytes>\n');
  process.exit(2);
}
const base = await brotliBytes(basePath);
const candidate = await brotliBytes(candidatePath);
const delta = candidate - base;
process.stdout.write(`Brotli download: base ${base} B, candidate ${candidate} B, delta ${delta} B (budget ${budget} B)\n`);
if (delta > budget) {
  process.stderr.write(`The candidate package exceeds its size budget by ${delta - budget} B\n`);
  process.exit(1);
}
