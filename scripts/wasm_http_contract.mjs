// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// The HTTP contract of a deployment package (WASM_DEPLOYMENT.md "Required HTTP contract"): which
// file a request names, and the headers, cache classes and representation of the response. No Node
// imports, so the reference server (serve_wasm.mjs) and the hosted Worker (packaging/web) share it.

export const CACHE_CONTROL = {
  revalidate: 'no-cache',
  immutable: 'public, max-age=31536000, immutable',
};
export const ISOLATION_HEADERS = {
  'Cross-Origin-Opener-Policy': 'same-origin',
  'Cross-Origin-Embedder-Policy': 'require-corp',
  'Cross-Origin-Resource-Policy': 'same-origin',
  'X-Content-Type-Options': 'nosniff',
};

// The decoded path of a request URL, or undefined when it is malformed.
export function decodedPathname(requestUrl) {
  try {
    return decodeURIComponent(new URL(requestUrl, 'http://localhost').pathname);
  } catch {
    return undefined;
  }
}

// The package-relative file a decoded request path names ('/' is the entrypoint), or undefined for
// one that is empty or could leave the package.
export function safeLogicalPath(pathname, entrypoint) {
  const logicalPath = pathname === '/' ? entrypoint : pathname.replace(/^\/+/, '');
  return logicalPath && !logicalPath.includes('\\') && !logicalPath.split('/').includes('..')
    ? logicalPath : undefined;
}

// The file entry for manifest.json, which the manifest does not list.
export function manifestFile(identity) {
  return {
    path: 'manifest.json',
    contentType: 'application/json; charset=utf-8',
    cache: 'revalidate',
    representations: { identity },
  };
}

export function parseAcceptEncoding(header) {
  const qualities = new Map();
  for (const item of (header || '').split(',')) {
    const [rawName, ...parameters] = item.trim().toLowerCase().split(';');
    if (!rawName) {
      continue;
    }
    let quality = 1;
    for (const parameter of parameters) {
      const match = parameter.trim().match(/^q=(0(?:\.\d{0,3})?|1(?:\.0{0,3})?)$/);
      if (match) {
        quality = Number(match[1]);
      }
    }
    qualities.set(rawName, quality);
  }
  return qualities;
}

export function selectRepresentation(file, acceptEncoding) {
  const qualities = parseAcceptEncoding(acceptEncoding);
  const wildcard = qualities.get('*');
  const identityQuality = qualities.get('identity') ?? (wildcard === 0 ? 0 : 1);
  const candidates = [
    ['br', file.representations.br, qualities.get('br') ?? wildcard ?? 0, 3],
    ['gzip', file.representations.gzip, qualities.get('gzip') ?? wildcard ?? 0, 2],
    ['identity', file.representations.identity, identityQuality, 1],
  ].filter(([, representation, quality]) => representation && quality > 0);
  candidates.sort((left, right) => right[2] - left[2] || right[3] - left[3]);
  return candidates[0]?.[1];
}

export function etag(representation) {
  return `"sha256-${representation.sha256}"`;
}

export function conditionalMatch(header, value) {
  return (header || '').split(',').map(candidate => candidate.trim()).some(
    candidate => candidate === '*' || candidate === value || candidate === `W/${value}`,
  );
}

// The headers of a response with the `selected` representation of `file`, without Content-Length.
// A representation without a hash (the Worker does not hash manifest.json) carries no ETag.
export function responseHeaders(file, selected) {
  const headers = {
    ...ISOLATION_HEADERS,
    'Cache-Control': CACHE_CONTROL[file.cache],
    'Content-Type': file.contentType,
  };
  if (selected.sha256) {
    headers.ETag = etag(selected);
  }
  if (file.representations.br || file.representations.gzip) {
    headers.Vary = 'Accept-Encoding';
  }
  if (selected.contentEncoding) {
    headers['Content-Encoding'] = selected.contentEncoding;
  }
  return headers;
}
