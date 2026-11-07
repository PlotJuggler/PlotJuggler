#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Usage: node scripts/wasm_source_offer.mjs [--output <dir>] [--links] [--dry-run]
//
// Writes the corresponding source of the browser build, which is conveyed under the GPL version 3, and the written
// offer that names it: PlotJuggler 4 itself (the HEAD of this checkout), Qt, Qt Advanced Docking System, and every
// dependency the build fetches (each FetchContent_Declare pin, CPython, and the Abseil that protobuf fetches). Run it
// once per deployment, from a clean checkout of the built revision and after scripts/package_wasm.mjs (which replaces
// its output directory), into <deployment>/sources, where the THIRD_PARTY_NOTICES.txt of the build points. Writes
// ~300 MB. A pinned hash is verified (a mismatch, a missing hash, or a Qt version without hashes fails); a git pin is
// resolved to its commit and archived from GitHub's tarball of that commit, whose hash is recorded. With --links, only
// PlotJuggler 4 (a private repository) is archived: the offer gives every library's public address instead, which
// the GPL accepts beside the object code as long as those addresses keep serving it; each is checked to answer.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { createReadStream, createWriteStream } from 'node:fs';
import { mkdir, readdir, readFile, rename, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { fileURLToPath } from 'node:url';

import { readVersion } from './wasm_manifest.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ADS_DIRECTORY = '3rdparty/Qt-Advanced-Docking';
const ISSUES = 'https://github.com/PlotJuggler/PJ4/issues';
// protobuf fetches it in its own build, at the abseil-cpp-version of its cmake/dependencies.cmake.
const ABSEIL = 'https://github.com/abseil/abseil-cpp.git';
// The Qt modules on pj_app's link line (the SBOMs read by cmake/PjWasmNotices.cmake), and the libraries each provides.
const QT_MODULES = {
  qtbase: 'Core, Gui, Widgets, Network, OpenGL, OpenGLWidgets, PrintSupport, Xml, Concurrent and the plugins',
  qtsvg: 'Svg, SvgWidgets and their plugins',
  qttools: 'UiTools',
};
// SHA-256 of Qt's source archives per release: the .sha256 file beside each on download.qt.io.
const QT_SHA256 = {
  '6.12.0': {
    qtbase: 'a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307',
    qtsvg: 'e4ab39534ec97987b1b9b60ef7f4d3253d5912a69f8c713a01753174a4029331',
    qttools: '8dab8f3611496486a470ad5f115ceea584f36bc22a2b8b6f6ebdbafbb8160693',
  },
};

const read = file => readFile(path.join(ROOT, file), 'utf8');
const isCommit = tag => /^[0-9a-f]{40}$/.test(tag);
const git = (...args) => execFileSync('git', args, { cwd: ROOT, encoding: 'utf8' }).trim();

function gitArchive(directory, { name, commit }, output) {
  try {
    execFileSync('git', ['-C', directory, 'archive', '--format=tar.gz', `--prefix=${name.replace(/\.tar\.gz$/, '')}/`,
      '-o', path.join(output, name), commit], { stdio: 'pipe' });
  } catch (error) {
    throw new Error(`cannot archive ${commit} of ${directory} (a submodule needs git submodule update --init)\n`
      + `${error.stderr}`, { cause: error });
  }
}

// CMake text without comments (a # outside a quoted string runs to the end of the line).
const stripComments = text => text.replace(/("[^"\n]*")|#[^\n]*/g, (match, quoted) => quoted ?? '');

function expand(value, variables) {
  return value.replace(/\$\{(\w+)\}/g, (reference, name) => {
    if (!variables.has(name)) {
      throw new Error(`cannot resolve ${reference} in "${value}"`);
    }
    return expand(variables.get(name), variables);
  });
}

// Every FetchContent_Declare of the browser build, with the pin it carries: its URL and URL_HASH, or GIT_REPOSITORY and
// GIT_TAG. A ${variable} resolves from the file's own set() calls. The three declarations of plotjuggler_sdk collapse
// to the one with a pin (the first is a local directory).
export async function declaredDependencies() {
  const cmake = (await readdir(path.join(ROOT, 'cmake'))).filter(name => /^PjWasm.*\.cmake$/.test(name));
  const files = [...cmake.map(name => `cmake/${name}`), 'CMakeLists.txt', '3rdparty/qwt/CMakeLists.txt'];
  // The root CMakeLists.txt derives the SDK version from the conanfile.
  const sdkVersion = (await read('conanfile.txt')).match(/^plotjuggler_sdk\/([0-9][0-9A-Za-z.-]*)/m)?.[1];
  const found = new Map();
  for (const file of files) {
    const text = stripComments(await read(file));
    const variables = new Map([['PJ_SDK_VERSION', sdkVersion]]);
    for (const [, name, value] of text.matchAll(/\bset\(\s*(\w+)\s+([^\s)]+)\s*\)/g)) {
      variables.set(name, value);
    }
    for (const [, name, args] of text.matchAll(/FetchContent_Declare\(\s*([A-Za-z0-9_.+-]+)([^)]*)\)/g)) {
      const tokens = (args.match(/"[^"]*"|\S+/g) ?? []).map(token => token.replace(/^"|"$/g, ''));
      const value = key => (tokens.includes(key) ? expand(tokens[tokens.indexOf(key) + 1], variables) : undefined);
      const dependency = {
        name,
        file,
        url: value('URL'),
        urlHash: value('URL_HASH'),
        repository: value('GIT_REPOSITORY'),
        tag: value('GIT_TAG'),
      };
      const known = found.get(name);
      if (!known || (!known.url && !known.repository)) {
        found.set(name, dependency);
      }
    }
  }
  return [...found.values()].sort((left, right) => left.name.localeCompare(right.name));
}

export function dependencyArchive({ name, file, url, urlHash, repository, tag }) {
  const short = name.replace(/^pj_wasm_/, '').replace(/_upstream$/, '');
  const what = `${short}, pinned in ${file}`;
  const named = base => (base.toLowerCase().startsWith(short.toLowerCase()) ? base : `${short}-${base}`);
  if (url) {
    const sha256 = urlHash?.match(/^SHA256=([0-9a-f]{64})$/)?.[1];
    if (!sha256) {
      throw new Error(`${name} (${file}) has no URL_HASH SHA256=<hash>`);
    }
    const base = path.posix.basename(new URL(url).pathname);
    return {
      dependency: name, name: named(/\.(tar\.\w+|tgz|zip)$/.test(base) ? base : `${base}.tar.gz`), url, sha256, what,
    };
  }
  if (repository && tag) {
    const label = isCommit(tag) ? tag.slice(0, 12) : tag.replaceAll('/', '_');
    return { dependency: name, name: `${short}-${label}.tar.gz`, repository, tag, what };
  }
  throw new Error(`${name} (${file}) has no URL or GIT_TAG pin`);
}

export async function planSourceOffer({ qtVersion } = {}) {
  const qt = qtVersion ?? await readVersion('PJ_QT_VERSION');
  const hashes = QT_SHA256[qt];
  if (!hashes) {
    throw new Error(`no pinned SHA-256 for Qt ${qt}: add the .sha256 of its qtbase, qtsvg and qttools archives `
      + '(download.qt.io/official_releases/qt) to QT_SHA256');
  }
  const series = qt.split('.').slice(0, 2).join('.');
  const archives = Object.entries(QT_MODULES).map(([module, libraries]) => {
    const name = `${module}-everywhere-src-${qt}.tar.xz`;
    return {
      dependency: module,
      name,
      // The archive tree keeps every release; official_releases drops a superseded one.
      url: `https://download.qt.io/archive/qt/${series}/${qt}/submodules/${name}`,
      sha256: hashes[module],
      what: `${module}: ${libraries}`,
    };
  });
  for (const dependency of await declaredDependencies()) {
    archives.push(dependencyArchive(dependency));
  }
  const python = await read('cmake/PjWasmPython.cmake');
  const version = python.match(/^set\(_pj_cpython_version (\S+)\)/m)?.[1];
  const sha256 = python.match(/^set\(_pj_cpython_sha256 ([0-9a-f]{64})\)/m)?.[1];
  if (!version || !sha256) {
    throw new Error('cmake/PjWasmPython.cmake does not pin CPython as set(_pj_cpython_version ...) and _sha256');
  }
  archives.push({
    dependency: 'cpython',
    name: `Python-${version}.tar.xz`,
    url: `https://www.python.org/ftp/python/${version}/Python-${version}.tar.xz`,
    sha256,
    what: 'CPython, pinned in cmake/PjWasmPython.cmake',
  });
  archives.push({
    dependency: 'absl',
    name: 'abseil-cpp.tar.gz',
    repository: ABSEIL,
    pinnedBy: 'protobuf',
    what: "Abseil, at the abseil-cpp-version in protobuf's cmake/dependencies.cmake",
  });
  const commit = git('ls-tree', 'HEAD', ADS_DIRECTORY).split(/\s+/)[2];
  const adsUrl = git('config', '-f', '.gitmodules', '--get', `submodule.${ADS_DIRECTORY}.url`);
  const revision = git('rev-parse', 'HEAD');
  return {
    archives,
    pj4: { name: `PlotJuggler-4-${revision.slice(0, 12)}.tar.gz`, commit: revision },
    ads: {
      name: `Qt-Advanced-Docking-System-${commit.slice(0, 12)}.tar.gz`,
      commit,
      url: adsUrl,
      tarball: `https://codeload.github.com/${githubRepository(adsUrl)}/tar.gz/${commit}`,
    },
    revision,
    qt,
    emscripten: await readVersion('PJ_EMSCRIPTEN_VERSION'),
    node: await readVersion('PJ_WASM_NODE_VERSION'),
  };
}

export function renderSourceOffer({ archives, pj4, ads, revision, qt, emscripten, node }, { links = false } = {}) {
  const listing = archives.map(archive => [
    `  ${archive.name}`,
    `    ${archive.what}`,
    archive.repository
      && `    ${archive.tag && !isCommit(archive.tag) ? `tag ${archive.tag} of ` : ''}${archive.repository}`,
    archive.commit && `    commit ${archive.commit}`,
    archive.sha256 && `    SHA-256 ${archive.sha256}`,
    archive.url && `    from ${archive.url}`,
  ].filter(Boolean).join('\n')).join('\n\n');
  const where = links
    ? `The complete corresponding source is PlotJuggler 4, in this directory, and every library the module is built
from, at the public address below of the exact release or commit its build pins. It is also available on written
request via ${ISSUES}.`
    : `This directory holds the complete corresponding source: PlotJuggler 4 and every library the module is built
from, as the exact releases or commits its build pins. It is also available on written request via
${ISSUES}.`;
  const gitPins = links
    ? `A pin to a git tag is given as the commit the tag named when this offer was written, with GitHub's tarball of
it (an entry's name labels its download; only PlotJuggler 4's archive is in this directory).`
    : `A pin to a git tag is archived as GitHub's tarball of the commit the tag named when this directory was written,
with the SHA-256 it had then.`;
  return `PlotJuggler 4, browser build: corresponding source and written offer

The browser build (plotjuggler4.wasm, plotjuggler4.js, qtloader.js) is a combined work conveyed under the GNU General
Public License version 3, because it statically links Qt's WebAssembly platform plugin, which Qt provides only under
the GPL version 3 or a commercial licence. PlotJuggler 4's own source is MPL-2.0, which permits this combination; the
desktop application remains MPL-2.0. The libraries it links keep their own licences:

  - Qt ${qt}: the GNU LGPL version 3, except the WebAssembly platform plugin (qwasm), as above;
  - FFmpeg (libavcodec, libavutil, libswscale), built LGPL-only: the GNU LGPL version 2.1 or later;
  - the Qt Advanced Docking System, PlotJuggler's fork: the GNU LGPL version 2.1 or later.

${where}
The licence texts, the GPL version 3 among them, are in THIRD_PARTY_NOTICES.txt, at the root of the deployment and
behind the About dialog's "Third-party licences" link.

  ${pj4.name}
    PlotJuggler 4: \`git archive\` of revision ${pj4.commit}, including the
    build scripts and the libraries vendored under 3rdparty/; also at https://github.com/PlotJuggler/PJ4

${listing}

${links ? `  Qt Advanced Docking System, PlotJuggler's fork with its changes: the 3rdparty/Qt-Advanced-Docking
    submodule commit ${ads.commit} of ${ads.url.replace(/\.git$/, '')}
    from ${ads.tarball}` : `  ${ads.name}
    Qt Advanced Docking System, PlotJuggler's fork with its changes: \`git archive\` of the
    3rdparty/Qt-Advanced-Docking submodule commit ${ads.commit},
    also at ${ads.url.replace(/\.git$/, '')}`}

Qt and FFmpeg are unmodified upstream releases. The Qt modules came as The Qt Company's prebuilt WebAssembly
(multithread) kit, built from the archives above; FFmpeg is compiled from its archive with configure options only.
${gitPins}
The libraries are named with their licences in THIRD_PARTY_NOTICES.txt. The Emscripten
runtime linked into the module (musl, libc++, compiler-rt, the zlib port) comes with the Emscripten SDK ${emscripten}
pinned in versions.env, whose source is public at https://github.com/emscripten-core/emscripten; it is not copied here.

Relinking against a modified library
------------------------------------
PlotJuggler 4 (the archive above, revision ${revision} for this deployment)
pins everything the module is built from:

  - versions.env: Qt ${qt}, Emscripten ${emscripten}, Node ${node};
  - cmake/PjWasm*.cmake: the archives above, and the FFmpeg and CPython configure options;
  - the 3rdparty/Qt-Advanced-Docking submodule: the commit above;
  - .github/workflows/wasm-ci.yml: the commands that configure and build the production package; and
  - scripts/package_wasm.mjs: the packaging that turns the build into this deployment.

To use a modified library, build the module from that source with the library replaced, then package it:

  - Qt: build the modules from the archives above, following Qt's instructions for WebAssembly with threads
    (https://doc.qt.io/qt-6/wasm.html), and give the PlotJuggler 4 configure the toolchain file and host Qt of that
    install (the -DCMAKE_TOOLCHAIN_FILE and -DQT_HOST_PATH lines of wasm-ci.yml);
  - FFmpeg and every other library fetched by the build: point its URL and URL_HASH, or GIT_REPOSITORY and GIT_TAG, in
    cmake/PjWasm*.cmake (the root CMakeLists.txt for the SDK) at the modified source;
  - Qt Advanced Docking System: replace the contents of 3rdparty/Qt-Advanced-Docking.

The object files of the module are not published; they are produced by that build.
`;
}

async function fileSha256(file) {
  const hash = createHash('sha256');
  await pipeline(createReadStream(file), hash);
  return hash.digest('hex');
}

async function fetchSha256(url, file) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`HTTP ${response.status}`);
  }
  const hash = createHash('sha256');
  await pipeline(Readable.fromWeb(response.body), async function* (chunks) {
    for await (const chunk of chunks) {
      hash.update(chunk);
      yield chunk;
    }
  }, createWriteStream(file));
  return hash.digest('hex');
}

// Downloads `archive` into `directory`, trying up to six times (ffmpeg.org is slow enough to time out a connection).
// A pinned hash is verified; a git pin has none, so its hash is recorded.
async function download(archive, directory) {
  const target = path.join(directory, archive.name);
  if (archive.sha256 && await fileSha256(target).catch(() => '') === archive.sha256) {
    return;
  }
  const partial = `${target}.part`;
  let actual;
  for (let attempt = 1; actual === undefined; attempt += 1) {
    try {
      actual = await fetchSha256(archive.url, partial);
    } catch (error) {
      if (attempt === 6) {
        throw new Error(`${archive.url}: ${error.message} (${error.cause?.message ?? 'no cause'})`, { cause: error });
      }
      await new Promise(resolve => setTimeout(resolve, 2000 * attempt));
    }
  }
  if (archive.sha256 && actual !== archive.sha256) {
    await rm(partial);
    throw new Error(`${archive.name}: SHA-256 is ${actual}, the pinned value is ${archive.sha256}`);
  }
  archive.sha256 = actual;
  await rename(partial, target);
}

const tarList = file => execFileSync('tar', ['-tzf', file], { encoding: 'utf8', maxBuffer: 1 << 28 })
  .split('\n').filter(Boolean);

// Whether the tarball's top-level .gitmodules (which a repository may carry empty) lists a submodule.
function hasSubmodules(file) {
  const member = tarList(file).find(entry => /^[^/]+\/\.gitmodules$/.test(entry));
  const gitmodules = member && execFileSync('tar', ['-xzOf', file, member], { encoding: 'utf8' });
  return /^\[submodule /m.test(gitmodules ?? '');
}

// The commit a tag names; an annotated tag is peeled to its commit.
function resolveTag(repository, tag) {
  if (isCommit(tag)) {
    return tag;
  }
  const refs = execFileSync('git', ['ls-remote', repository, `refs/tags/${tag}`, `refs/tags/${tag}^{}`],
    { encoding: 'utf8' }).split('\n').filter(Boolean).map(line => line.split(/\s+/));
  const ref = refs.find(([, name]) => name.endsWith('^{}')) ?? refs[0];
  if (!ref) {
    throw new Error(`${repository} has no tag ${tag}`);
  }
  return ref[0];
}

function githubRepository(url) {
  const repository = url.match(/^https:\/\/github\.com\/([^/]+\/[^/]+?)(?:\.git)?$/)?.[1];
  if (!repository) {
    throw new Error(`${url} is not a GitHub repository, so it has no tarball to archive`);
  }
  return repository;
}

// Fills in what a git pin needs before it downloads: the tag protobuf pins for Abseil (read from protobuf's resolved
// commit, which comes earlier in the plan), the commit, GitHub's tarball.
async function resolveGitPin(archive, archives) {
  if (archive.pinnedBy) {
    const pinner = archives.find(entry => entry.dependency === archive.pinnedBy);
    const repository = githubRepository(pinner.repository);
    const file = `https://raw.githubusercontent.com/${repository}/${pinner.commit}/cmake/dependencies.cmake`;
    const response = await fetch(file);
    archive.tag = response.ok ? (await response.text()).match(/set\(abseil-cpp-version "([^"]+)"\)/)?.[1] : undefined;
    if (!archive.tag) {
      throw new Error(`${file} does not set abseil-cpp-version (HTTP ${response.status})`);
    }
  }
  archive.commit = resolveTag(archive.repository, archive.tag);
  archive.url = `https://codeload.github.com/${githubRepository(archive.repository)}/tar.gz/${archive.commit}`;
}

// GitHub's tarball has no submodules, which the build's git clone would have fetched: refuse a pin that has any.
async function refuseSubmodules(archive) {
  const repository = githubRepository(archive.repository);
  const response = await fetch(`https://raw.githubusercontent.com/${repository}/${archive.commit}/.gitmodules`);
  if (!response.ok && response.status !== 404) {
    throw new Error(`cannot read the .gitmodules of ${archive.name} (HTTP ${response.status})`);
  }
  if (response.ok && /^\[submodule /m.test(await response.text())) {
    throw new Error(`${archive.name} has git submodules, which GitHub's tarball of the commit does not contain`);
  }
}

// Fails unless `url` answers, trying three times: an offer must not point at an address that serves nothing.
async function checkReachable(url) {
  for (let attempt = 1; ; attempt += 1) {
    const status = await fetch(url, { method: 'HEAD' }).then(response => response.status, error => error.message);
    if (status === 200) {
      return;
    }
    if (attempt === 3) {
      throw new Error(`${url} does not answer (${status})`);
    }
    await new Promise(resolve => setTimeout(resolve, 2000 * attempt));
  }
}

export async function writeSourceOffer({ output, links = false, dryRun = false, log = () => {} }) {
  const plan = await planSourceOffer();
  const names = links
    ? [plan.pj4.name, 'SOURCE-OFFER.txt']
    : [plan.pj4.name, ...plan.archives.map(archive => archive.name), plan.ads.name, 'SOURCE-OFFER.txt'];
  log(`${dryRun ? 'Would write' : 'Writing'} to ${output}:\n${names.map(name => `  ${name}`).join('\n')}\n`);
  if (dryRun) {
    return plan;
  }
  await mkdir(output, { recursive: true });
  if (links) {
    for (const archive of plan.archives) {
      log(`  ${archive.name}`);
      if (archive.repository) {
        await resolveGitPin(archive, plan.archives);
        await refuseSubmodules(archive);
      }
      await checkReachable(archive.url);
    }
    await checkReachable(plan.ads.tarball);
    gitArchive(ROOT, plan.pj4, output);
    await writeFile(path.join(output, 'SOURCE-OFFER.txt'), renderSourceOffer(plan, { links }));
    return plan;
  }
  for (const archive of plan.archives) {
    log(`  ${archive.name}`);
    if (archive.repository) {
      await resolveGitPin(archive, plan.archives);
    }
    await download(archive, output);
    // GitHub's tarball has no submodules, which the build's git clone would have fetched.
    if (archive.repository && hasSubmodules(path.join(output, archive.name))) {
      throw new Error(`${archive.name} has git submodules, which GitHub's tarball of the commit does not contain`);
    }
  }
  gitArchive(ROOT, plan.pj4, output);
  gitArchive(path.join(ROOT, ADS_DIRECTORY), plan.ads, output);
  await writeFile(path.join(output, 'SOURCE-OFFER.txt'), renderSourceOffer(plan));
  return plan;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  const option = name => (args.includes(name) ? args[args.indexOf(name) + 1] : undefined);
  if (args.includes('--help')
      || args.some(arg => arg.startsWith('--') && !['--output', '--links', '--dry-run'].includes(arg))) {
    process.stdout.write('Usage: node scripts/wasm_source_offer.mjs [--output <dir>] [--links] [--dry-run]\n'
      + '  --output <dir>  Where to write the sources (default: build-wasm/deploy/sources)\n'
      + '  --links         Archive PlotJuggler 4 only; give every library its public address\n'
      + '  --dry-run       List the files and download nothing\n');
    process.exit(args.includes('--help') ? 0 : 2);
  }
  writeSourceOffer({
    output: path.resolve(option('--output') ?? path.join(ROOT, 'build-wasm', 'deploy', 'sources')),
    links: args.includes('--links'),
    dryRun: args.includes('--dry-run'),
    log: message => process.stdout.write(`${message}\n`),
  }).catch(error => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}
