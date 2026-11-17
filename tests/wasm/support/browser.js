// SPDX-License-Identifier: MPL-2.0

// Shared launch options for every browser suite, per Playwright browser.
//
// The offline rules are a containment boundary: the suites boot the real application dozens of times per
// run on US-hosted CI runners, so a startup network call the product makes would reach a real server and
// look like user traffic there. Every request that is not for the local fixture server must die before it
// reaches the network.
//
// Chromium: every name except loopback fails resolution. ~NOTFOUND, not an address: MAP to 0.0.0.0 is not
// a dead end, because the kernel rewrites a connect() to the unspecified address as a connect() to
// loopback, which serves a foreign origin from the fixture server whenever the ports coincide.
//
// Firefox has no resolver override, so every non-loopback HTTP(S)/WebSocket request goes to a dead proxy
// (port 9, discard). Playwright's Firefox profile lets the proxy hijack localhost; that is switched off so
// the fixture server stays reachable directly.
//
// page.route() interception is unaffected: a fulfilled route never reaches the network stack.
const OFFLINE_RESOLVER_RULES = 'MAP * ~NOTFOUND, EXCLUDE 127.0.0.1, EXCLUDE localhost, EXCLUDE [::1]';

const FIREFOX_OFFLINE_PREFS = {
  'network.proxy.type': 1,
  'network.proxy.http': '127.0.0.1',
  'network.proxy.http_port': 9,
  'network.proxy.ssl': '127.0.0.1',
  'network.proxy.ssl_port': 9,
  'network.proxy.share_proxy_settings': true,
  'network.proxy.allow_hijacking_localhost': false,
  'network.proxy.no_proxies_on': 'localhost, 127.0.0.1, [::1]',
};

function launchOptions(browserName = 'chromium', extraFirefoxPrefs = {}) {
  if (browserName === 'firefox') {
    return { firefoxUserPrefs: { ...FIREFOX_OFFLINE_PREFS, ...extraFirefoxPrefs } };
  }
  const options = { args: [`--host-resolver-rules=${OFFLINE_RESOLVER_RULES}`] };
  if (process.env.PJ_CHROMIUM_EXECUTABLE) {
    options.executablePath = process.env.PJ_CHROMIUM_EXECUTABLE;
  }
  return options;
}

// Playwright projects for every supported browser, sharing the offline launch options.
function browserProjects() {
  return ['chromium', 'firefox'].map(browserName => ({
    name: browserName,
    use: { browserName, launchOptions: launchOptions(browserName) },
  }));
}

// Expected typeof window.showOpenFilePicker / showSaveFilePicker: Firefox has no File System Access API,
// so the <input>/download fallback is all there is.
function nativePickerType(browserName) {
  return browserName === 'firefox' ? 'undefined' : 'function';
}

// test.use() options for a device pixel ratio other than 1. Firefox drops Playwright's deviceScaleFactor
// override when the page switches to its cross-origin-isolated (COOP/COEP) process, so there the page
// comes from a browser launched with the layout.css.devPixelsPerPx pref. The `context` fixture is
// replaced with one from that browser, so no default browser is launched and thrown away (Playwright
// rejects a describe-level override of `browser` itself).
function devicePixelRatioOptions(ratio) {
  return {
    deviceScaleFactor: ratio,
    context: async ({ playwright, browserName, viewport }, use) => {
      const extraPrefs = browserName === 'firefox' ? { 'layout.css.devPixelsPerPx': String(ratio) } : {};
      const browser = await playwright[browserName].launch(launchOptions(browserName, extraPrefs));
      try {
        await use(await browser.newContext({ viewport, deviceScaleFactor: ratio }));
      } finally {
        await browser.close();
      }
    },
  };
}

module.exports = { launchOptions, browserProjects, nativePickerType, devicePixelRatioOptions, OFFLINE_RESOLVER_RULES };
