#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import { createReadStream } from 'node:fs';
import { readFile, stat } from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  CACHE_CONTROL, ISOLATION_HEADERS, conditionalMatch, decodedPathname, manifestFile, responseHeaders,
  safeLogicalPath, selectRepresentation,
} from './wasm_http_contract.mjs';
import { sha256 } from './wasm_manifest.mjs';

function usage() {
  return `Usage: node scripts/serve_wasm.mjs [options]

Options:
  --root <dir>  Deployment package directory (default: build-wasm/deploy)
  --host <host> Bind address (default: 127.0.0.1)
  --port <port> TCP port, or 0 for an ephemeral port (default: 6931)
  --help        Show this help
`;
}

function parseArguments(argv) {
  const scriptDir = path.dirname(fileURLToPath(import.meta.url));
  const options = {
    root: path.resolve(scriptDir, '..', 'build-wasm', 'deploy'),
    host: '127.0.0.1',
    port: 6931,
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--help') {
      process.stdout.write(usage());
      process.exit(0);
    }
    if (!['--root', '--host', '--port'].includes(argument)) {
      throw new Error(`Unknown argument: ${argument}\n\n${usage()}`);
    }
    const value = argv[index + 1];
    if (!value || value.startsWith('--')) {
      throw new Error(`${argument} requires a value`);
    }
    options[argument.slice(2)] = value;
    index += 1;
  }
  options.root = path.resolve(options.root);
  options.port = Number(options.port);
  if (!Number.isInteger(options.port) || options.port < 0 || options.port > 65535) {
    throw new Error('--port must be an integer from 0 through 65535');
  }
  return options;
}

async function loadDeployment(root) {
  const manifestPath = path.join(root, 'manifest.json');
  const manifestBytes = await readFile(manifestPath);
  const manifest = JSON.parse(manifestBytes.toString('utf8'));
  if (manifest.format !== 'org.plotjuggler.wasm-deployment.v1') {
    throw new Error(`Unsupported deployment manifest format: ${manifest.format}`);
  }
  if (!Array.isArray(manifest.files) || manifest.entrypoint !== 'index.html') {
    throw new Error('Deployment manifest has no supported entrypoint/file table');
  }
  for (const [name, value] of Object.entries(ISOLATION_HEADERS)) {
    if (manifest.requiredResponseHeaders?.[name] !== value) {
      throw new Error(`Deployment manifest has an invalid required header: ${name}`);
    }
  }
  for (const [name, value] of Object.entries(CACHE_CONTROL)) {
    if (manifest.cachePolicies?.[name] !== value) {
      throw new Error(`Deployment manifest has an invalid cache policy: ${name}`);
    }
  }
  const headersTemplate = manifest.deploymentConfiguration?.headersTemplate;
  if (headersTemplate?.path !== '_headers'
      || !Number.isSafeInteger(headersTemplate.bytes)
      || !/^[0-9a-f]{64}$/.test(headersTemplate.sha256)) {
    throw new Error('Deployment manifest has no valid _headers metadata');
  }
  const headersBytes = await readFile(path.join(root, '_headers'));
  const headersHash = sha256(headersBytes);
  if (headersBytes.length !== headersTemplate.bytes || headersHash !== headersTemplate.sha256) {
    throw new Error('Deployment _headers does not match its manifest metadata');
  }

  const files = new Map();
  for (const file of manifest.files) {
    if (!file.path || path.isAbsolute(file.path) || file.path.split('/').includes('..')) {
      throw new Error(`Unsafe manifest path: ${file.path}`);
    }
    if (!CACHE_CONTROL[file.cache] || !file.contentType || !file.representations?.identity) {
      throw new Error(`Incomplete manifest entry: ${file.path}`);
    }
    if (files.has(file.path) || file.representations.identity.path !== file.path) {
      throw new Error(`Duplicate or indirect identity manifest entry: ${file.path}`);
    }
    const expectedEncodings = { identity: undefined, br: 'br', gzip: 'gzip' };
    for (const [name, representation] of Object.entries(file.representations)) {
      if (!(name in expectedEncodings)
          || representation.contentEncoding !== expectedEncodings[name]
          || !Number.isSafeInteger(representation.bytes)
          || representation.bytes < 0
          || !/^[0-9a-f]{64}$/.test(representation.sha256)) {
        throw new Error(`Invalid ${name} representation: ${file.path}`);
      }
      const diskPath = path.resolve(root, representation.path);
      if (!diskPath.startsWith(`${root}${path.sep}`)) {
        throw new Error(`Representation escapes deployment root: ${representation.path}`);
      }
      const metadata = await stat(diskPath);
      if (!metadata.isFile() || metadata.size !== representation.bytes) {
        throw new Error(`Representation size mismatch: ${representation.path}`);
      }
    }
    files.set(file.path, file);
  }

  files.set('manifest.json', manifestFile({
    path: 'manifest.json',
    bytes: manifestBytes.length,
    sha256: sha256(manifestBytes),
  }));
  return { manifest, files };
}

function sendError(response, statusCode, message) {
  const body = Buffer.from(`${message}\n`);
  response.writeHead(statusCode, {
    ...ISOLATION_HEADERS,
    'Cache-Control': 'no-store',
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Length': body.length,
  });
  response.end(body);
}

export async function createWasmServer(rootDirectory) {
  const root = path.resolve(rootDirectory);
  const deployment = await loadDeployment(root);

  return http.createServer((request, response) => {
    void (async () => {
      if (!['GET', 'HEAD'].includes(request.method)) {
        response.setHeader('Allow', 'GET, HEAD');
        sendError(response, 405, 'Method not allowed');
        return;
      }

      const pathname = decodedPathname(request.url);
      if (pathname === undefined) {
        sendError(response, 400, 'Malformed request URL');
        return;
      }
      const logicalPath = safeLogicalPath(pathname, deployment.manifest.entrypoint);
      if (!logicalPath) {
        sendError(response, 404, 'Not found');
        return;
      }

      const file = deployment.files.get(logicalPath);
      if (!file) {
        sendError(response, 404, 'Not found');
        return;
      }
      const selected = selectRepresentation(file, request.headers['accept-encoding']);
      if (!selected) {
        sendError(response, 406, 'No acceptable content encoding');
        return;
      }

      const headers = responseHeaders(file, selected);
      if (conditionalMatch(request.headers['if-none-match'], headers.ETag)) {
        response.writeHead(304, headers);
        response.end();
        return;
      }

      response.writeHead(200, { ...headers, 'Content-Length': selected.bytes });
      if (request.method === 'HEAD') {
        response.end();
        return;
      }
      const stream = createReadStream(path.join(root, selected.path));
      stream.on('error', error => response.destroy(error));
      stream.pipe(response);
    })().catch(error => {
      if (!response.headersSent) {
        sendError(response, 500, 'Internal server error');
      } else {
        response.destroy(error);
      }
    });
  });
}

export async function listenWasmServer(root, { host = '127.0.0.1', port = 0 } = {}) {
  const server = await createWasmServer(root);
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
  return server;
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const server = await listenWasmServer(options.root, options);
  const address = server.address();
  const printableHost = address.address.includes(':') ? `[${address.address}]` : address.address;
  process.stdout.write(`PJ_WASM_SERVER_URL=http://${printableHost}:${address.port}/\n`);

  const stop = () => server.close(() => process.exit(0));
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => {
    process.stderr.write(`${error.stack || error}\n`);
    process.exitCode = 1;
  });
}
