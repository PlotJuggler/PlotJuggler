// SPDX-License-Identifier: MPL-2.0
// A robot serves the browser app itself: Caddy, configured by docs/wasm/Caddyfile, serves the
// deployment package over HTTPS and proxies /bridge to the PlotJuggler Bridge, so a preset whose
// stream URL is ${deployment.ws_origin}/bridge works on any robot host. Each case runs the real
// Caddyfile (PJ_CADDY, else caddy on PATH), configured only through its environment, in front of
// the acceptance package (PJ_WASM_ACCEPTANCE_PACKAGE_DIR) and the fake bridge, with a throwaway
// CA. Chromium maps the robot's name to loopback and trusts exactly its certificate.
const { execFileSync, spawn } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { chromium, test, expect } = require('@playwright/test');
const { observeRuntimeTarget, waitForQtApp } = require('./support/app');
const { launchOptions, OFFLINE_RESOLVER_RULES } = require('./support/browser');
const { COUNTER_AND_SINE, drawnPlots, robotPreset, startBridgeServer } = require('./support/bridge');
const { latest, layoutOutcome: layoutOutcomeOf } = require('./support/console');
const {
  expectEntrypoint,
  expectServedAsDeclared,
  expectUndeclaredHidden,
  rawRequest,
} = require('./support/deployment_contract');
const { datasetCount } = require('./support/preset_server');

const REPOSITORY = path.join(__dirname, '..', '..');
const CADDYFILE = path.join(REPOSITORY, 'docs', 'wasm', 'Caddyfile');
const PACKAGE = path.resolve(
  process.env.PJ_WASM_ACCEPTANCE_PACKAGE_DIR || path.join(REPOSITORY, 'build-wasm-probe', 'deploy'));
const HOST = 'robot.test';
const PRESET_INDEX = {
  presets: [
    { id: 'robot', title: 'Robot', layout: 'robot/layout.pj4.xml' },
    { id: 'signals', title: 'Signals', layout: 'signals/layout.pj4.xml' },
    // Written by the case that needs it: its URL names that case's bridge port.
    { id: 'plain', title: 'Plain WebSocket', layout: 'plain/layout.pj4.xml' },
  ],
};

test.skip(({ browserName }) => browserName !== 'chromium',
  'Only Chromium can map a test host name and trust a single certificate from the command line.');

let work;
let site;
// Everything a case started, closed after it in reverse order. A case that times out never
// resumes, and a Caddy or bridge child process would otherwise outlive the run.
const running = [];
function track(resource) {
  running.push(resource);
  return resource;
}

// A throwaway CA and a certificate it signs for HOST. `spki` is the certificate key's
// base64 SHA-256, which Chromium is told to trust.
function makeCertificates(directory) {
  const file = name => path.join(directory, name);
  const openssl = (...args) => execFileSync('openssl', args, { stdio: 'pipe' });
  const key = ['-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:prime256v1', '-nodes'];
  openssl('req', '-x509', ...key, '-keyout', file('ca.key'), '-out', file('ca.crt'), '-days', '2',
    '-subj', '/CN=PlotJuggler test CA', '-addext', 'basicConstraints=critical,CA:TRUE',
    '-addext', 'keyUsage=critical,keyCertSign');
  openssl('req', ...key, '-keyout', file('robot.key'), '-out', file('robot.csr'), '-subj', `/CN=${HOST}`);
  fs.writeFileSync(file('robot.ext'), `subjectAltName=DNS:${HOST}\nextendedKeyUsage=serverAuth\n`);
  openssl('x509', '-req', '-in', file('robot.csr'), '-CA', file('ca.crt'), '-CAkey', file('ca.key'),
    '-CAcreateserial', '-days', '2', '-extfile', file('robot.ext'), '-out', file('robot.crt'));
  const publicKey = new crypto.X509Certificate(fs.readFileSync(file('robot.crt'))).publicKey;
  return {
    ca: fs.readFileSync(file('ca.crt')),
    tls: `${file('robot.crt')} ${file('robot.key')}`,
    spki: crypto.createHash('sha256').update(publicKey.export({ type: 'spki', format: 'der' })).digest('base64'),
  };
}

// The site a robot serves: the package, plus a presets/ folder written here. Caddy serves what is
// on disk, so links to the package's entries stand in for unpacking a copy. `typeFiles` are
// manifest-style entries for one file of every deployable type, each extension also in capitals;
// the compressible types carry .br and .gz siblings, as the packager writes them for presets.
async function makeSiteRoot(directory) {
  const { COMPRESSIBLE_TYPES, CONTENT_TYPES } = await import('../../scripts/package_wasm.mjs');
  const { representation } = await import('../../scripts/wasm_manifest.mjs');
  const root = path.join(directory, 'site');
  fs.mkdirSync(path.join(root, 'presets', 'robot'), { recursive: true });
  for (const name of fs.readdirSync(PACKAGE).filter(entry => entry !== 'presets')) {
    fs.symlinkSync(path.join(PACKAGE, name), path.join(root, name));
  }
  const write = (relative, contents) => {
    fs.mkdirSync(path.dirname(path.join(root, relative)), { recursive: true });
    fs.writeFileSync(path.join(root, relative), contents);
  };
  write('presets/index.json', JSON.stringify(PRESET_INDEX));
  write('presets/robot/layout.pj4.xml', robotPreset('${deployment.ws_origin}/bridge', COUNTER_AND_SINE));
  fs.cpSync(path.join(__dirname, 'fixtures', 'signals'), path.join(root, 'presets', 'signals'),
    { recursive: true });
  const typeFiles = [];
  for (const [extension, contentType] of CONTENT_TYPES) {
    for (const name of new Set([`sample${extension}`, `SAMPLE${extension.toUpperCase()}`])) {
      const logicalPath = `presets/types/${name}`;
      const contents = Buffer.from(`${name}\n`);
      write(logicalPath, contents);
      const representations = { identity: representation(logicalPath, contents) };
      if (COMPRESSIBLE_TYPES.has(extension)) {
        const encodings = [
          ['br', `${logicalPath}.br`, zlib.brotliCompressSync(contents)],
          ['gzip', `${logicalPath}.gz`, zlib.gzipSync(contents)],
        ];
        for (const [encoding, encodedPath, encoded] of encodings) {
          write(encodedPath, encoded);
          representations[encoding] = representation(encodedPath, encoded, encoding);
        }
      }
      typeFiles.push({ path: logicalPath, contentType, cache: 'revalidate', representations });
    }
  }
  return { root, typeFiles };
}

test.beforeAll(async () => {
  if (!fs.existsSync(path.join(PACKAGE, 'manifest.json'))) {
    throw new Error(`No acceptance package at ${PACKAGE}: package build-wasm-probe/pj_app with `
      + 'scripts/package_wasm.mjs, or set PJ_WASM_ACCEPTANCE_PACKAGE_DIR');
  }
  if (fs.existsSync(path.join(PACKAGE, 'presets'))) {
    throw new Error(`${PACKAGE} deploys presets; this spec needs a package without them, beside its own`);
  }
  work = fs.mkdtempSync(path.join(os.tmpdir(), 'pj-robot-'));
  site = { ...makeCertificates(work), ...await makeSiteRoot(work) };
});

test.afterEach(async () => {
  for (const resource of running.splice(0).reverse()) {
    await resource.close();
  }
});

test.afterAll(() => {
  if (work) {
    fs.rmSync(work, { recursive: true, force: true });
  }
});

// Ports nothing listens on now, on every interface (Caddy binds them all).
async function freePorts(count) {
  const servers = await Promise.all(Array.from({ length: count }, () => new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, () => resolve(server));
  })));
  const ports = servers.map(server => server.address().port);
  await Promise.all(servers.map(server => new Promise(resolve => server.close(resolve))));
  return ports;
}

// Runs docs/wasm/Caddyfile with `env` (tracked from its spawn) and resolves once it serves.
async function startCaddy(env) {
  const caddy = spawn(process.env.PJ_CADDY || 'caddy', ['run', '--config', CADDYFILE, '--adapter', 'caddyfile'],
    { env: { ...process.env, ...env }, stdio: ['ignore', 'ignore', 'pipe'] });
  const exited = new Promise(resolve => caddy.once('close', resolve));
  track({
    close: async () => {
      caddy.kill();
      await exited;
    },
  });
  let log = '';
  await new Promise((resolve, reject) => {
    caddy.once('error', error => reject(new Error(`Cannot run Caddy (set PJ_CADDY or put caddy on PATH): ${error}`)));
    // 'close' once stderr is drained, so the log ends with Caddy's own error.
    caddy.once('close', code => reject(new Error(`Caddy exited with ${code}:\n${log}`)));
    caddy.stderr.on('data', (chunk) => {
      log += chunk;
      if (log.includes('serving initial configuration')) {
        resolve();
      }
    });
  });
}

// The robot: the fake bridge, and Caddy in front of it and of the site.
async function startRobot() {
  const bridge = track(await startBridgeServer());
  const [httpsPort, httpPort, adminPort] = await freePorts(3);
  const state = fs.mkdtempSync(path.join(work, 'caddy-'));
  await startCaddy({
    PJ_ROBOT_HOST: `${HOST}:${httpsPort}`,
    PJ_HTTP_PORT: String(httpPort),
    PJ_WASM_ROOT: site.root,
    PJ_TLS: site.tls,
    PJ_BRIDGE_UPSTREAM: `127.0.0.1:${bridge.port}`,
    // Its own admin endpoint and storage: parallel runs never share :2019 or ~/.local/share/caddy.
    CADDY_ADMIN: `127.0.0.1:${adminPort}`,
    XDG_DATA_HOME: state,
    XDG_CONFIG_HOME: state,
  });
  return { origin: `https://${HOST}:${httpsPort}`, httpOrigin: `http://${HOST}:${httpPort}`, bridge };
}

// Node's half of --host-resolver-rules: the robot's name is loopback.
function toLoopback(hostname, options, callback) {
  if (options.all) {
    callback(null, [{ address: '127.0.0.1', family: 4 }]);
  } else {
    callback(null, '127.0.0.1', 4);
  }
}

async function launchChromium({ trustRobot }) {
  return track(await chromium.launch({
    ...launchOptions('chromium'),
    args: [
      `--host-resolver-rules=MAP ${HOST} 127.0.0.1, ${OFFLINE_RESOLVER_RULES}`,
      ...(trustRobot ? [`--ignore-certificate-errors-spki-list=${site.spki}`] : []),
    ],
  }));
}

async function boot(browser, url) {
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  page.on('filechooser', () => {});
  const run = { page, messages: [], errors: [], responses: new Map(), sockets: [] };
  observeRuntimeTarget(page, run.messages, [], run.errors);
  page.on('response', response => run.responses.set(response.url(), response));
  page.on('websocket', socket => run.sockets.push(socket.url()));
  await page.goto(url);
  await waitForQtApp(page, run.messages);
  return run;
}

// The PJ_WASM_LAYOUT_LOAD_* outcome of the layout at `url`.
async function layoutOutcome(run, url) {
  const outcome = () => layoutOutcomeOf(run.messages, url);
  await expect.poll(outcome, { timeout: 60000 }).not.toBe('');
  return outcome();
}

test('Caddy serves the package exactly as its manifest declares', async () => {
  test.setTimeout(120000);
  const robot = await startRobot();
  const trusted = { ca: site.ca, lookup: toLoopback };
  const manifest = JSON.parse(fs.readFileSync(path.join(PACKAGE, 'manifest.json'), 'utf8'));
  await expectEntrypoint(robot.origin, manifest, trusted);
  for (const file of [...manifest.files, ...site.typeFiles]) {
    await expectServedAsDeclared(robot.origin, file, trusted);
  }
  await expectUndeclaredHidden(robot.origin, manifest, trusted);
  // Plain HTTP on the robot only redirects to the HTTPS page.
  const redirect = await rawRequest(`${robot.httpOrigin}/?preset=robot`, { lookup: toLoopback });
  expect(redirect.status).toBe(308);
  expect(redirect.headers.location).toBe(`${robot.origin}/?preset=robot`);
});

test('the app boots cross-origin isolated over HTTPS and opens a preset with its data', async () => {
  test.setTimeout(180000);
  const robot = await startRobot();
  const browser = await launchChromium({ trustRobot: true });
  const run = await boot(browser, `${robot.origin}/?preset=signals`);
  expect(await run.page.evaluate(() => [isSecureContext, crossOriginIsolated])).toEqual([true, true]);
  expect(await layoutOutcome(run, `${robot.origin}/presets/signals/layout.pj4.xml`))
    .toContain('PJ_WASM_LAYOUT_LOAD_OK');
  for (const file of ['index.json', 'signals/layout.pj4.xml', 'signals/data.csv']) {
    expect(run.responses.get(`${robot.origin}/presets/${file}`)?.status(), file).toBe(200);
  }
  expect(await datasetCount(run.page, run.messages)).toBe(1);
  expect(run.errors).toEqual([]);
});

test('a robot preset streams from the bridge over wss through /bridge', async () => {
  test.setTimeout(180000);
  const robot = await startRobot();
  const browser = await launchChromium({ trustRobot: true });
  const run = await boot(browser, `${robot.origin}/?preset=robot`);
  expect(await layoutOutcome(run, `${robot.origin}/presets/robot/layout.pj4.xml`))
    .toContain('PJ_WASM_LAYOUT_LOAD_OK');
  // ${deployment.ws_origin} became the page's own origin; Caddy carried the upgrade to the bridge.
  const bridgeUrl = `${robot.origin.replace('https:', 'wss:')}/bridge`;
  await expect.poll(() => run.sockets, { timeout: 15000 }).toEqual([bridgeUrl]);
  await expect.poll(() => robot.bridge.seen('open').map(event => event.path), { timeout: 15000 })
    .toEqual(['/bridge']);
  const drawnTitles = async () => (await drawnPlots(run.page, run.messages)).map(plot => plot.titles);
  await expect.poll(drawnTitles, { timeout: 30000 }).toEqual([['robot/counter/value'], ['robot/sine/value']]);
  const [counter] = await drawnPlots(run.page, run.messages);
  await expect.poll(async () => (await drawnPlots(run.page, run.messages))[0].view.top, { timeout: 15000 })
    .toBeGreaterThan(counter.view.top);
  expect(run.errors).toEqual([]);
});

function trackServer(server) {
  track({
    close: () => new Promise((resolve) => {
      server.close(resolve);
      server.closeAllConnections();
    }),
  });
  return server;
}

// A static server that sends no isolation headers, as `python -m http.server` does. With `headOnly` it sends them
// on HEAD requests alone: the page then sees a server that asks for isolation and a page that did not get it, which
// is what a browser without cross-origin isolation (an Android WebView) shows.
async function listenWithoutIsolation(root, { headOnly = false } = {}) {
  const server = http.createServer((request, response) => {
    const pathname = new URL(request.url, 'http://localhost').pathname;
    fs.readFile(path.join(root, pathname === '/' ? 'index.html' : pathname), (error, body) => {
      const headers = headOnly && request.method === 'HEAD'
        ? { 'cross-origin-opener-policy': 'same-origin', 'cross-origin-embedder-policy': 'require-corp' }
        : {};
      response.writeHead(error ? 404 : 200, headers);
      response.end(request.method === 'HEAD' ? undefined : body);
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  return trackServer(server);
}

test('a page that is not secure, not isolated, or in a browser that cannot isolate it says why it cannot start', async () => {
  test.setTimeout(120000);
  const requirement = 'The app must be served over HTTPS, or from localhost, with the headers '
    + 'Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp.';
  // A robot without Caddy: the package's own headers, but plain HTTP from the robot's name.
  // (Caddy itself only redirects plain HTTP.)
  const { listenWasmServer } = await import('../../scripts/serve_wasm.mjs');
  const withHeaders = trackServer(await listenWasmServer(PACKAGE));
  const withoutHeaders = await listenWithoutIsolation(PACKAGE);
  const unsupportedBrowser = await listenWithoutIsolation(PACKAGE, { headOnly: true });
  const browser = await launchChromium({ trustRobot: false });
  const cases = [
    [`http://${HOST}:${withHeaders.address().port}/`, [false, false],
      `PlotJuggler cannot start: this page was not served over HTTPS. ${requirement}`],
    [`http://127.0.0.1:${withoutHeaders.address().port}/`, [true, false],
      `PlotJuggler cannot start: this page is not cross-origin isolated. ${requirement}`],
    [`http://127.0.0.1:${unsupportedBrowser.address().port}/`, [true, false],
      'PlotJuggler cannot start in this browser: it does not isolate the page as the app needs, although the server '
      + 'asks it to. Open it in a current Chrome, Edge, Firefox or Safari.'],
  ];
  for (const [url, context, message] of cases) {
    const page = await browser.newPage();
    const errors = [];
    const requests = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('request', request => requests.push(request.url()));
    await page.goto(url);
    expect(await page.evaluate(() => [isSecureContext, crossOriginIsolated]), url).toEqual(context);
    // The preflight returns in the block that sets this text: once it shows, the runtime never loads.
    await expect(page.locator('#status')).toHaveText(message, { timeout: 30000 });
    await expect(page.locator('#status')).toBeVisible();
    expect(requests.filter(request => request.includes('plotjuggler4.wasm')), url).toEqual([]);
    expect(errors, url).toEqual([]);
    await page.close();
  }
});

test('the browser refuses a robot certificate the device does not trust', async () => {
  test.setTimeout(60000);
  const robot = await startRobot();
  const browser = await launchChromium({ trustRobot: false });
  const page = await browser.newPage();
  await expect(page.goto(`${robot.origin}/`)).rejects.toThrow(/net::ERR_CERT_AUTHORITY_INVALID/);
});

test('a ws:// bridge URL on the HTTPS page fails its stream with a visible reason', async () => {
  test.setTimeout(180000);
  const robot = await startRobot();
  const plainUrl = `ws://${HOST}:${robot.bridge.port}/bridge`;
  const folder = path.join(site.root, 'presets', 'plain');
  fs.mkdirSync(folder, { recursive: true });
  fs.writeFileSync(path.join(folder, 'layout.pj4.xml'), robotPreset(plainUrl, COUNTER_AND_SINE));
  const browser = await launchChromium({ trustRobot: true });
  const run = await boot(browser, `${robot.origin}/?preset=plain`);
  // The load itself succeeds: it never waits for a connection.
  expect(await layoutOutcome(run, `${robot.origin}/presets/plain/layout.pj4.xml`))
    .toContain('PJ_WASM_LAYOUT_LOAD_OK');
  // Mixed content: the browser refuses the socket before any connection is made.
  const report = () => latest(run.messages, 'PJ_WASM_LAYOUT_DIALOG present=');
  await expect.poll(async () => {
    await run.page.evaluate(() => window.pjWasmReportLayoutDialogProbe());
    return report();
  }, { timeout: 30000 }).toContain('present=1');
  expect(report()).toContain('title=Streaming');
  // What the user reads, word for word.
  expect(report()).toContain(`body=The stream failed:\n\nplugin: failed to connect to PJ bridge at ${plainUrl}: `
    + `the browser refused to open a WebSocket to ${plainUrl}`);
  expect(run.messages.some(message => message.includes('the browser refused to open the WebSocket:')
    && message.includes('SecurityError'))).toBe(true);
  expect(run.sockets).toEqual([]);
  expect(robot.bridge.seen('connect')).toEqual([]);
  expect(run.errors).toEqual([]);
});
