#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import { createHash } from 'node:crypto';
import {
  access,
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rename,
  rm,
  writeFile,
} from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import {
  brotliCompress,
  constants as zlibConstants,
  gzip,
} from 'node:zlib';

import { representation, sha256 } from './wasm_manifest.mjs';

const brotliCompressAsync = promisify(brotliCompress);
const gzipAsync = promisify(gzip);

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPOSITORY_ROOT = path.resolve(SCRIPT_DIR, '..');
const REQUIRED_ARTIFACTS = [
  'plotjuggler4.js',
  'plotjuggler4.wasm',
  'qtloader.js',
  'qtlogo.svg',
];
// Every build writes it (cmake/PjWasmNotices.cmake); packaged beside the module, never referenced by the page.
const NOTICES = 'THIRD_PARTY_NOTICES.txt';
const CONTENT_TYPES = new Map([
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.json', 'application/json; charset=utf-8'],
  ['.svg', 'image/svg+xml'],
  ['.txt', 'text/plain; charset=utf-8'],
  ['.wasm', 'application/wasm'],
]);
const COMPRESSIBLE_TYPES = new Set(['.html', '.js', '.svg', '.txt', '.wasm']);
const MINIMUM_COMPRESSED_SIZE = 1024;
const BROTLI_QUALITY = 9;
const BROTLI_WINDOW = 22;
const GZIP_LEVEL = 9;
const DEPLOYMENT_FORMAT = 'org.plotjuggler.wasm-deployment.v1';

function usage() {
  return `Usage: node scripts/package_wasm.mjs [options]

Options:
  --source <dir>   Qt/Emscripten output directory (default: build-wasm/pj_app)
  --output <dir>   Deployment package directory (default: build-wasm/deploy)
  --version <text> Application version (default: PJ_VERSION or versions.env)
  --help           Show this help
`;
}

function parseArguments(argv) {
  const options = {
    source: path.join(REPOSITORY_ROOT, 'build-wasm', 'pj_app'),
    output: path.join(REPOSITORY_ROOT, 'build-wasm', 'deploy'),
    version: process.env.PJ_VERSION || '',
  };

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--help') {
      process.stdout.write(usage());
      process.exit(0);
    }
    if (!['--source', '--output', '--version'].includes(argument)) {
      throw new Error(`Unknown argument: ${argument}\n\n${usage()}`);
    }
    const value = argv[index + 1];
    if (!value || value.startsWith('--')) {
      throw new Error(`${argument} requires a value`);
    }
    options[argument.slice(2)] = value;
    index += 1;
  }

  options.source = path.resolve(options.source);
  options.output = path.resolve(options.output);
  return options;
}

async function defaultVersion() {
  const contents = await readFile(path.join(REPOSITORY_ROOT, 'versions.env'), 'utf8');
  const match = contents.match(/^PJ_APP_VERSION=(.+)$/m);
  if (!match || !match[1].trim()) {
    throw new Error('versions.env does not define PJ_APP_VERSION');
  }
  return match[1].trim();
}

function containsPath(parent, candidate) {
  const relative = path.relative(parent, candidate);
  return relative === ''
    || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
}

function contentType(fileName) {
  const value = CONTENT_TYPES.get(path.extname(fileName));
  if (!value) {
    throw new Error(`No deployment MIME type is defined for ${fileName}`);
  }
  return value;
}

function replaceExactlyOnce(contents, source, replacement) {
  const first = contents.indexOf(source);
  if (first < 0 || contents.indexOf(source, first + source.length) >= 0) {
    throw new Error(`Expected exactly one ${JSON.stringify(source)} in plotjuggler4.html`);
  }
  return `${contents.slice(0, first)}${replacement}${contents.slice(first + source.length)}`;
}

async function compressedRepresentations(contents) {
  const brotli = await brotliCompressAsync(contents, {
    params: {
      [zlibConstants.BROTLI_PARAM_MODE]: zlibConstants.BROTLI_MODE_GENERIC,
      // Quality 9 keeps release packaging practical for the 250+ MiB module;
      // quality 11 took more than five CPU-minutes without completing on the
      // reference host. Binary-size work is measured separately from delivery.
      [zlibConstants.BROTLI_PARAM_QUALITY]: BROTLI_QUALITY,
      [zlibConstants.BROTLI_PARAM_LGWIN]: BROTLI_WINDOW,
    },
  });
  const gzipContents = await gzipAsync(contents, { level: GZIP_LEVEL, mtime: 0 });

  // Normalize the gzip OS byte as well as mtime so output is byte-identical on
  // Linux, macOS, and Windows Node runtimes. RFC 1952 defines 255 as unknown.
  gzipContents.fill(0, 4, 8);
  gzipContents[9] = 255;
  return { brotli, gzip: gzipContents };
}

async function writeAsset(stagingRoot, logicalPath, contents, compress) {
  const destination = path.join(stagingRoot, logicalPath);
  await mkdir(path.dirname(destination), { recursive: true });
  await writeFile(destination, contents, { mode: 0o644 });

  const representations = {
    identity: representation(logicalPath, contents),
  };
  if (compress && contents.length >= MINIMUM_COMPRESSED_SIZE) {
    const encoded = await compressedRepresentations(contents);
    const brotliPath = `${logicalPath}.br`;
    const gzipPath = `${logicalPath}.gz`;
    await writeFile(path.join(stagingRoot, brotliPath), encoded.brotli, { mode: 0o644 });
    await writeFile(path.join(stagingRoot, gzipPath), encoded.gzip, { mode: 0o644 });
    representations.br = representation(brotliPath, encoded.brotli, 'br');
    representations.gzip = representation(gzipPath, encoded.gzip, 'gzip');
  }

  return {
    path: logicalPath,
    contentType: contentType(logicalPath),
    cache: logicalPath.startsWith('assets/') ? 'immutable' : 'revalidate',
    representations,
  };
}

function deploymentHeaders(bundlePath) {
  return `# Generated by scripts/package_wasm.mjs. Used by hosts which support
# the Netlify/Cloudflare Pages _headers format. Precompressed representation
# selection must still be enabled by the hosting platform.
/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  Cross-Origin-Resource-Policy: same-origin
  X-Content-Type-Options: nosniff

/
  Cache-Control: no-cache

/index.html
  Cache-Control: no-cache

/manifest.json
  Cache-Control: no-cache

/${bundlePath}/*
  Cache-Control: public, max-age=31536000, immutable
`;
}

async function validateSource(sourceRoot) {
  const required = ['plotjuggler4.html', ...REQUIRED_ARTIFACTS, NOTICES];
  await Promise.all(required.map(async fileName => {
    const candidate = path.join(sourceRoot, fileName);
    try {
      await access(candidate);
    } catch {
      throw new Error(`Required Qt/WASM artifact is missing: ${candidate}`);
    }
  }));

  const [javascript, wasm] = await Promise.all([
    readFile(path.join(sourceRoot, 'plotjuggler4.js'), 'utf8'),
    readFile(path.join(sourceRoot, 'plotjuggler4.wasm')),
  ]);
  // Emscripten preserves these semantic identifiers but removes whitespace and
  // unquotes the memory descriptor in optimized builds. Keep the validation
  // equally strict for Release and diagnostic output without coupling it to a
  // particular pretty-printing mode.
  const hasPthreadRuntime = /\b(?:var|let|const)\s+PThread\s*=\s*\{\s*unusedWorkers\s*:/.test(javascript);
  const hasSharedMemory = /(?:["']shared["']|\bshared)\s*:\s*true\b/.test(javascript);
  if (!hasPthreadRuntime || !hasSharedMemory) {
    throw new Error('plotjuggler4.js is not a threaded Emscripten runtime');
  }
  if (wasm.length < 8 || !wasm.subarray(0, 4).equals(Buffer.from([0x00, 0x61, 0x73, 0x6d]))) {
    throw new Error('plotjuggler4.wasm does not have a WebAssembly magic header');
  }
}

async function validateReplaceableOutput(outputRoot) {
  let entries;
  try {
    entries = await readdir(outputRoot);
  } catch (error) {
    if (error.code === 'ENOENT') {
      return;
    }
    throw new Error(
      `Refusing to replace ${outputRoot}: output must be absent, an empty directory, or a prior PlotJuggler WASM package`,
      { cause: error },
    );
  }
  if (entries.length === 0) {
    return;
  }

  try {
    const manifest = JSON.parse(await readFile(path.join(outputRoot, 'manifest.json'), 'utf8'));
    if (manifest.format === DEPLOYMENT_FORMAT) {
      return;
    }
  } catch {
    // The refusal below deliberately treats unreadable/malformed manifests as
    // unrelated content rather than guessing that the directory is ours.
  }
  throw new Error(
    `Refusing to replace ${outputRoot}: non-empty output is not a prior PlotJuggler WASM package`,
  );
}

export async function packageWasm(options) {
  const sourceRoot = path.resolve(options.source);
  const outputRoot = path.resolve(options.output);
  const version = options.version || await defaultVersion();
  if (!version.trim()) {
    throw new Error('The package version must not be empty');
  }
  if (outputRoot === path.parse(outputRoot).root
      || containsPath(sourceRoot, outputRoot)
      || containsPath(outputRoot, sourceRoot)) {
    throw new Error('The source and output directories must be distinct, non-nested paths');
  }
  await validateReplaceableOutput(outputRoot);
  await validateSource(sourceRoot);

  const sourceAssets = await Promise.all([...REQUIRED_ARTIFACTS, NOTICES].map(async fileName => ({
    fileName,
    contents: await readFile(path.join(sourceRoot, fileName)),
  })));
  const bundleDigest = createHash('sha256');
  for (const asset of sourceAssets) {
    bundleDigest.update(asset.fileName);
    bundleDigest.update('\0');
    bundleDigest.update(String(asset.contents.length));
    bundleDigest.update('\0');
    bundleDigest.update(sha256(asset.contents));
    bundleDigest.update('\n');
  }
  const bundleId = `sha256-${bundleDigest.digest('hex').slice(0, 32)}`;
  const bundlePath = `assets/${bundleId}`;
  await mkdir(path.dirname(outputRoot), { recursive: true });
  const stagingRoot = await mkdtemp(`${outputRoot}.tmp-`);

  try {
    const files = [];
    for (const asset of sourceAssets) {
      const logicalPath = `${bundlePath}/${asset.fileName}`;
      files.push(await writeAsset(
        stagingRoot,
        logicalPath,
        asset.contents,
        COMPRESSIBLE_TYPES.has(path.extname(asset.fileName)),
      ));
    }

    let entrypoint = await readFile(path.join(sourceRoot, 'plotjuggler4.html'), 'utf8');
    for (const fileName of REQUIRED_ARTIFACTS.filter(name => name !== 'plotjuggler4.wasm')) {
      entrypoint = replaceExactlyOnce(
        entrypoint,
        `src="${fileName}"`,
        `src="${bundlePath}/${fileName}"`,
      );
    }
    const qtLoadMarker = 'const instance = await qtLoad({';
    entrypoint = replaceExactlyOnce(
      entrypoint,
      qtLoadMarker,
      `${qtLoadMarker}\n                    locateFile: filename => filename === 'plotjuggler4.wasm'`
        + ` ? '${bundlePath}/plotjuggler4.wasm' : filename,`,
    );
    files.push(await writeAsset(
      stagingRoot,
      'index.html',
      Buffer.from(entrypoint),
      false,
    ));
    files.sort((left, right) => left.path.localeCompare(right.path));
    const headersContents = Buffer.from(deploymentHeaders(bundlePath));

    const manifest = {
      format: DEPLOYMENT_FORMAT,
      application: 'PlotJuggler 4',
      version: version.trim(),
      entrypoint: 'index.html',
      bundleId,
      packagingRuntime: {
        node: process.version,
        zlib: process.versions.zlib,
        brotli: { quality: BROTLI_QUALITY, window: BROTLI_WINDOW },
        gzip: { level: GZIP_LEVEL, mtime: 0, os: 255 },
      },
      deploymentConfiguration: {
        headersTemplate: representation('_headers', headersContents),
      },
      requiredResponseHeaders: {
        'Cross-Origin-Opener-Policy': 'same-origin',
        'Cross-Origin-Embedder-Policy': 'require-corp',
        'Cross-Origin-Resource-Policy': 'same-origin',
        'X-Content-Type-Options': 'nosniff',
      },
      cachePolicies: {
        revalidate: 'no-cache',
        immutable: 'public, max-age=31536000, immutable',
      },
      files,
    };
    await writeFile(
      path.join(stagingRoot, 'manifest.json'),
      `${JSON.stringify(manifest, null, 2)}\n`,
      { mode: 0o644 },
    );
    await writeFile(
      path.join(stagingRoot, '_headers'),
      headersContents,
      { mode: 0o644 },
    );

    // Re-check immediately before replacement so a directory populated while
    // compression was running is never deleted as an unrelated output path.
    await validateReplaceableOutput(outputRoot);
    await rm(outputRoot, { recursive: true, force: true });
    await rename(stagingRoot, outputRoot);
    return manifest;
  } catch (error) {
    await rm(stagingRoot, { recursive: true, force: true });
    throw error;
  }
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const manifest = await packageWasm(options);
  process.stdout.write(
    `Packaged ${manifest.application} ${manifest.version} as ${manifest.bundleId} in ${options.output}\n`,
  );
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => {
    process.stderr.write(`${error.stack || error}\n`);
    process.exitCode = 1;
  });
}
