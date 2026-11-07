#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import { createHash } from 'node:crypto';
import {
  access,
  lstat,
  mkdir,
  mkdtemp,
  readdir,
  readFile,
  rename,
  rm,
  stat,
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

import { CACHE_CONTROL, ISOLATION_HEADERS } from './wasm_http_contract.mjs';
import { readVersion, representation, sha256 } from './wasm_manifest.mjs';

const brotliCompressAsync = promisify(brotliCompress);
const gzipAsync = promisify(gzip);

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPOSITORY_ROOT = path.resolve(SCRIPT_DIR, '..');
const REQUIRED_ARTIFACTS = [
  'plotjuggler4.js',
  'plotjuggler4.wasm',
  'qtloader.js',
];
// The page icon of the entry page (Qt's template qtlogo.svg is not packaged).
const LOGO = 'plotjuggler.svg';
// Every build writes it (cmake/PjWasmNotices.cmake). Packaged beside the module, and again at the deployment root,
// where the About dialog opens it by this name (pj_app/src/ui/AboutDialog.ui).
const NOTICES = 'THIRD_PARTY_NOTICES.txt';
// Every deployable type. docs/wasm/Caddyfile repeats this table, and
// tests/wasm/robot_deployment.spec.js checks that Caddy serves each type as declared.
export const CONTENT_TYPES = new Map([
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.json', 'application/json; charset=utf-8'],
  ['.svg', 'image/svg+xml'],
  ['.txt', 'text/plain; charset=utf-8'],
  ['.wasm', 'application/wasm'],
  // Preset content: layouts, data, thumbnails, robot models and their notices.
  // MCAP has no registered media type.
  ['', 'text/plain; charset=utf-8'],
  ['.csv', 'text/csv; charset=utf-8'],
  ['.dae', 'model/vnd.collada+xml'],
  ['.glb', 'model/gltf-binary'],
  ['.gltf', 'model/gltf+json'],
  ['.mcap', 'application/octet-stream'],
  ['.md', 'text/markdown; charset=utf-8'],
  ['.obj', 'model/obj'],
  ['.png', 'image/png'],
  ['.stl', 'model/stl'],
  ['.xml', 'application/xml'],
]);
// Each preset path segment: portable on every static host and filesystem, and
// never a dotfile (.git, .DS_Store, editor state) or a URL-significant name.
const PRESET_SEGMENT = /^[A-Za-z0-9_-][A-Za-z0-9._-]*$/;
// Types that gain from .br/.gz: text, and the wasm module. Left out because they are compressed inside:
// .mcap (zstd chunks), .png, and .glb (embeds PNG/JPEG textures and Draco/meshopt buffers).
// The empty extension is the extensionless license/notice text of a preset package.
export const COMPRESSIBLE_TYPES = new Set([
  '', '.csv', '.dae', '.gltf', '.html', '.js', '.json', '.md', '.obj', '.stl', '.svg', '.txt', '.wasm', '.xml',
]);
const MINIMUM_COMPRESSED_SIZE = 1024;
const BROTLI_QUALITY = 9;
const BROTLI_WINDOW = 22;
const GZIP_LEVEL = 9;
const DEPLOYMENT_FORMAT = 'org.plotjuggler.wasm-deployment.v1';
// The entry page states it before the threaded runtime loads: without a secure,
// cross-origin isolated page (no SharedArrayBuffer) it would never start.
const ISOLATION_REQUIREMENT = 'The app must be served over HTTPS, or from localhost, with the headers '
  + 'Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp.';
// PlotJuggler's own entry page; Qt's generated plotjuggler4.html is not deployed.
const ENTRY_TEMPLATE = path.join(REPOSITORY_ROOT, 'packaging', 'web', 'loading.html.in');

function usage() {
  return `Usage: node scripts/package_wasm.mjs [options]

Options:
  --source <dir>   Qt/Emscripten output directory (default: build-wasm/pj_app)
  --output <dir>   Deployment package directory (default: build-wasm/deploy)
  --version <text> Application version (default: PJ_VERSION or versions.env)
  --presets <dir>  Preset tree to deploy under presets/ (default: none)
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
    if (!['--source', '--output', '--version', '--presets'].includes(argument)) {
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
  if (options.presets) {
    options.presets = path.resolve(options.presets);
  }
  return options;
}

function containsPath(parent, candidate) {
  const relative = path.relative(parent, candidate);
  return relative === ''
    || (!relative.startsWith(`..${path.sep}`) && relative !== '..' && !path.isAbsolute(relative));
}

// Extensions match in any case: robot packages often ship `.STL` meshes.
function contentType(fileName) {
  const value = CONTENT_TYPES.get(path.extname(fileName).toLowerCase());
  if (!value) {
    throw new Error(`No deployment MIME type is defined for ${fileName}`);
  }
  return value;
}

function isCompressible(fileName) {
  return COMPRESSIBLE_TYPES.has(path.extname(fileName).toLowerCase());
}

function fillTemplate(template, values) {
  return template.replace(/@([A-Z_]+)@/g, (placeholder, name) => {
    if (!(name in values)) {
      throw new Error(`${ENTRY_TEMPLATE} has an unknown placeholder ${placeholder}`);
    }
    return values[name];
  });
}

async function compressedRepresentations(contents) {
  const [brotli, gzipContents] = await Promise.all([
    brotliCompressAsync(contents, {
      params: {
        [zlibConstants.BROTLI_PARAM_MODE]: zlibConstants.BROTLI_MODE_GENERIC,
        // Quality 9 keeps release packaging practical for the 250+ MiB module;
        // quality 11 took more than five CPU-minutes without completing on the
        // reference host. Binary-size work is measured separately from delivery.
        [zlibConstants.BROTLI_PARAM_QUALITY]: BROTLI_QUALITY,
        [zlibConstants.BROTLI_PARAM_LGWIN]: BROTLI_WINDOW,
      },
    }),
    gzipAsync(contents, { level: GZIP_LEVEL, mtime: 0 }),
  ]);

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

// Lists every file of a preset tree as a '/'-separated path, refusing anything
// a static host should not publish: symlinks (they can point anywhere), special
// files, and names outside PRESET_SEGMENT. Throws before anything is written.
async function presetFiles(presetsRoot, prefix = '') {
  const files = [];
  for (const name of (await readdir(path.join(presetsRoot, prefix))).sort()) {
    const relative = prefix ? `${prefix}/${name}` : name;
    if (!PRESET_SEGMENT.test(name)) {
      throw new Error(`Refusing preset entry with an unsafe name: ${relative}`);
    }
    const metadata = await lstat(path.join(presetsRoot, relative));
    if (metadata.isDirectory()) {
      files.push(...await presetFiles(presetsRoot, relative));
    } else if (metadata.isFile()) {
      if (!CONTENT_TYPES.has(path.extname(name).toLowerCase())) {
        throw new Error(`Refusing preset entry with no deployment MIME type: ${relative}`);
      }
      files.push(relative);
    } else {
      throw new Error(`Refusing preset entry that is not a regular file or directory: ${relative}`);
    }
  }
  return files;
}

// Refuses a preset tree whose index.json the browser could not use: absent,
// malformed, without a presets array, with an entry the browser's catalog parser
// refuses (which drops every entry), or naming a relative layout or thumbnail
// the tree does not deploy. Absolute http(s) references live elsewhere and pass.
async function validatePresetIndex(presetsRoot, files) {
  let index;
  try {
    index = JSON.parse(await readFile(path.join(presetsRoot, 'index.json'), 'utf8'));
  } catch (error) {
    throw new Error(`The preset tree needs a readable JSON index.json: ${error.message}`, { cause: error });
  }
  if (!Array.isArray(index?.presets)) {
    throw new Error('The preset index.json has no presets array');
  }
  // The entry rules of pj_runtime's parsePresetCatalog(), which rejects the whole index for any of them.
  const ids = new Set();
  for (const [position, entry] of index.presets.entries()) {
    const label = `The preset index.json entry #${position + 1}`;
    if (entry === null || typeof entry !== 'object' || Array.isArray(entry)) {
      throw new Error(`${label} is not an object`);
    }
    const fields = ['id', 'title', 'description', 'layout', 'thumbnail'];
    if (fields.some(field => entry[field] !== undefined && entry[field] !== null && typeof entry[field] !== 'string')) {
      throw new Error(`${label} has a field that is not a string`);
    }
    if (!entry.id) {
      throw new Error(`${label} has no id`);
    }
    if (ids.has(entry.id)) {
      throw new Error(`The preset index.json has a duplicate preset id ${JSON.stringify(entry.id)}`);
    }
    ids.add(entry.id);
    if (!entry.layout) {
      throw new Error(`The preset index.json entry ${JSON.stringify(entry.id)} has no layout`);
    }
  }
  const deployed = new Set(files);
  const base = new URL('https://presets.invalid/presets/index.json');
  for (const entry of index.presets) {
    for (const reference of [entry?.layout, entry?.thumbnail].filter(value => value != null)) {
      const target = typeof reference === 'string' && URL.canParse(reference, base)
        ? new URL(reference, base) : undefined;
      if (target && target.origin !== base.origin && ['http:', 'https:'].includes(target.protocol)) {
        continue;
      }
      const relative = target?.origin === base.origin && target.pathname.startsWith('/presets/')
        ? decodeURIComponent(target.pathname.slice('/presets/'.length)) : undefined;
      if (!deployed.has(relative)) {
        throw new Error(`The preset index.json names a file the tree does not deploy: ${JSON.stringify(reference)}`);
      }
    }
  }
}

function headerRule(route, headers) {
  return `${route}\n${Object.entries(headers).map(([name, value]) => `  ${name}: ${value}\n`).join('')}`;
}

function deploymentHeaders(bundlePath, withPresets) {
  const revalidate = { 'Cache-Control': CACHE_CONTROL.revalidate };
  const rules = [
    headerRule('/*', ISOLATION_HEADERS),
    ...['/', '/index.html', '/manifest.json', `/${NOTICES}`].map(route => headerRule(route, revalidate)),
    headerRule(`/${bundlePath}/*`, { 'Cache-Control': CACHE_CONTROL.immutable }),
    ...(withPresets ? [headerRule('/presets/*', revalidate)] : []),
  ];
  return `# Generated by scripts/package_wasm.mjs. Used by hosts which support
# the Netlify/Cloudflare Pages _headers format. Precompressed representation
# selection must still be enabled by the hosting platform.
${rules.join('\n')}`;
}

async function validateSource(sourceRoot) {
  const required = [...REQUIRED_ARTIFACTS, NOTICES];
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
  const presetsRoot = options.presets ? path.resolve(options.presets) : undefined;
  const version = options.version || await readVersion('PJ_APP_VERSION');
  if (!version.trim()) {
    throw new Error('The package version must not be empty');
  }
  // Replacing the output deletes it, so no input may live inside it (or hold it).
  if (outputRoot === path.parse(outputRoot).root
      || [sourceRoot, presetsRoot].some(input => input
        && (containsPath(input, outputRoot) || containsPath(outputRoot, input)))) {
    throw new Error('The source, presets and output directories must be distinct, non-nested paths');
  }
  await validateReplaceableOutput(outputRoot);
  await validateSource(sourceRoot);
  if (presetsRoot && !(await stat(presetsRoot)).isDirectory()) {
    throw new Error(`The preset tree is not a directory: ${presetsRoot}`);
  }
  const presets = presetsRoot ? await presetFiles(presetsRoot) : [];
  if (presetsRoot) {
    await validatePresetIndex(presetsRoot, presets);
  }

  const sourceAssets = await Promise.all([
    ...[...REQUIRED_ARTIFACTS, NOTICES].map(async fileName => ({
      fileName,
      contents: await readFile(path.join(sourceRoot, fileName)),
    })),
    readFile(path.join(REPOSITORY_ROOT, 'resources', LOGO)).then(contents => ({ fileName: LOGO, contents })),
  ]);
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
        isCompressible(asset.fileName),
      ));
    }

    const notices = sourceAssets.find(asset => asset.fileName === NOTICES);
    files.push(await writeAsset(stagingRoot, NOTICES, notices.contents, true));

    const wasm = files.find(file => file.path === `${bundlePath}/plotjuggler4.wasm`).representations;
    const entrypoint = fillTemplate(await readFile(ENTRY_TEMPLATE, 'utf8'), {
      BUNDLE: bundlePath,
      VERSION: version.trim(),
      WASM_BYTES: String(wasm.identity.bytes),
      DOWNLOAD_MB: String(Math.max(1, Math.round((wasm.br ?? wasm.identity).bytes / 1e6))),
      ISOLATION_REQUIREMENT: JSON.stringify(ISOLATION_REQUIREMENT),
    });
    files.push(await writeAsset(
      stagingRoot,
      'index.html',
      Buffer.from(entrypoint),
      false,
    ));
    // Revalidated: a sample may change under the same name. The identity bytes are the tree's own, and
    // every encoding decodes to them, so a source's content_sha256 holds whichever one is served.
    for (const relative of presets) {
      files.push(await writeAsset(
        stagingRoot,
        `presets/${relative}`,
        await readFile(path.join(presetsRoot, relative)),
        isCompressible(relative),
      ));
    }
    // Code-unit order, as presetFiles walks: localeCompare would follow the locale.
    files.sort((left, right) => (left.path < right.path ? -1 : left.path > right.path ? 1 : 0));
    const headersContents = Buffer.from(deploymentHeaders(bundlePath, presets.length > 0));

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
      requiredResponseHeaders: ISOLATION_HEADERS,
      cachePolicies: CACHE_CONTROL,
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
