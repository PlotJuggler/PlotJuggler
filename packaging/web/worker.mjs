// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The hosted browser app (app.plotjuggler.io): serves one deployment package release from R2
// under the package's HTTP contract, as scripts/serve_wasm.mjs does locally, and keeps the
// telemetry service reachable.
//
// Bindings (wrangler.toml): SITE, the R2 bucket holding releases under `<RELEASE>/`; RELEASE, the
// live release prefix (set at deploy, so switching releases is one atomic deploy); ASSET_HOST, the
// asset host's bucket, which holds the samples' index at PRESET_INDEX_PATH; TELEMETRY, the telemetry
// Worker; STATS_HOST, the host that is the telemetry Worker for every path.
//
// `/presets/index.json`, the samples' index the app reads at boot, comes from ASSET_HOST at request
// time, never from the release: listing a sample is an upload (upload_preset_index.mjs), not a deploy.
//
// The telemetry Worker stays reachable two ways: STATS_HOST forwards every request to it (its
// dashboard), and `/telemetry` (and every path under it) on any host does, because installed
// desktop releases post their launch ping to https://app.plotjuggler.io/telemetry and their daily
// usage reports to https://app.plotjuggler.io/telemetry/v2/usage.

import {
  CACHE_CONTROL, ISOLATION_HEADERS, JSON_CONTENT_TYPE, PRESET_INDEX_PATH, conditionalMatch, decodedPathname,
  manifestFile, responseHeaders, safeLogicalPath, selectRepresentation,
} from '../../scripts/wasm_http_contract.mjs';

// The samples' index as a file of the contract: revalidated and never precompressed. R2's ETag stands in
// for the hash a manifest entry would carry.
const INDEX_FILE = { contentType: JSON_CONTENT_TYPE, cache: 'revalidate', representations: {} };

// The release's file table and entrypoint, kept per isolate: a release is immutable once uploaded.
let loaded = null;

async function fileTable(env) {
  if (loaded?.release === env.RELEASE) {
    return loaded;
  }
  const object = await env.SITE.get(`${env.RELEASE}/manifest.json`);
  if (!object) {
    throw new Error(`release ${env.RELEASE} has no manifest.json`);
  }
  const manifest = JSON.parse(await object.text());
  const files = new Map(manifest.files.map(file => [file.path, file]));
  // No hash: manifest.json is served without an ETag.
  files.set('manifest.json', manifestFile({ path: 'manifest.json', bytes: object.size, sha256: '' }));
  loaded = { release: env.RELEASE, files, entrypoint: manifest.entrypoint };
  return loaded;
}

function textResponse(status, message, extra = {}) {
  return new Response(`${message}\n`, {
    status,
    headers: {
      ...ISOLATION_HEADERS, 'Cache-Control': 'no-store', 'Content-Type': 'text/plain; charset=utf-8', ...extra,
    },
  });
}

// `/sources/...`: the GPL-3.0 source offer, published beside the release but outside its manifest.
async function sourceOffer(request, env, logicalPath) {
  const object = await env.SITE.get(`${env.RELEASE}/${logicalPath}`);
  if (!object) {
    return textResponse(404, 'Not found');
  }
  const contentType = logicalPath.endsWith('.txt') ? 'text/plain; charset=utf-8' : 'application/octet-stream';
  return new Response(request.method === 'HEAD' ? null : object.body, {
    headers: { ...ISOLATION_HEADERS, 'Cache-Control': CACHE_CONTROL.revalidate, 'Content-Type': contentType },
  });
}

// `/presets/index.json` from ASSET_HOST, revalidated by R2's ETag. A missing object is a 404, which the
// app takes as "no samples"; an R2 failure is an uncached 503, so the next request tries again.
async function presetIndex(request, env) {
  let object;
  try {
    object = await env.ASSET_HOST.get(PRESET_INDEX_PATH);
  } catch {
    return textResponse(503, 'Sample index unavailable');
  }
  if (!object) {
    return textResponse(404, 'Not found');
  }
  const headers = { ...responseHeaders(INDEX_FILE, {}), ETag: object.httpEtag };
  const notModified = conditionalMatch(request.headers.get('If-None-Match'), object.httpEtag);
  if (notModified || request.method === 'HEAD') {
    await object.body.cancel();
    return notModified ? new Response(null, { status: 304, headers })
      : new Response(null, { headers: { ...headers, 'Content-Length': String(object.size) } });
  }
  return new Response(object.body, { headers });
}

// The only export: workerd takes every named export of the entry module for an entrypoint and fails to start.
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.hostname === env.STATS_HOST || url.pathname === '/telemetry' || url.pathname.startsWith('/telemetry/')) {
      return env.TELEMETRY.fetch(request);
    }
    if (!['GET', 'HEAD'].includes(request.method)) {
      return textResponse(405, 'Method not allowed', { Allow: 'GET, HEAD' });
    }
    const pathname = decodedPathname(request.url);
    if (pathname === undefined) {
      return textResponse(400, 'Malformed request URL');
    }
    if (pathname === `/${PRESET_INDEX_PATH}`) {
      return presetIndex(request, env);
    }
    let table;
    try {
      table = await fileTable(env);
    } catch (error) {
      return textResponse(500, error.message);
    }
    const logicalPath = safeLogicalPath(pathname, table.entrypoint);
    if (!logicalPath) {
      return textResponse(404, 'Not found');
    }
    if (logicalPath.startsWith('sources/')) {
      return sourceOffer(request, env, logicalPath);
    }
    const file = table.files.get(logicalPath);
    if (!file) {
      return textResponse(404, 'Not found');
    }
    // The runtime rewrites Accept-Encoding to what the edge accepts; request.cf keeps the client's own.
    const accepted = request.cf?.clientAcceptEncoding ?? request.headers.get('Accept-Encoding');
    const selected = selectRepresentation(file, accepted);
    if (!selected) {
      return textResponse(406, 'No acceptable content encoding');
    }

    const headers = responseHeaders(file, selected);
    if (headers.ETag && conditionalMatch(request.headers.get('If-None-Match'), headers.ETag)) {
      return new Response(null, { status: 304, headers });
    }
    if (request.method === 'HEAD') {
      return new Response(null, { headers: { ...headers, 'Content-Length': String(selected.bytes) } });
    }
    const object = await env.SITE.get(`${env.RELEASE}/${selected.path}`);
    if (!object) {
      return textResponse(500, 'Release file missing');
    }
    // `manual`: the body is already encoded; the runtime must send it as it is.
    return new Response(object.body, { headers, encodeBody: 'manual' });
  },
};
