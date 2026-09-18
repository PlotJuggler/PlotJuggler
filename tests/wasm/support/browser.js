// SPDX-License-Identifier: MPL-2.0

// Shared Chromium launch options for every browser suite.
//
// The resolver rule is a containment boundary, not a convenience: these suites
// boot the real application dozens of times per run on US-hosted CI runners, so
// any startup network call the product makes would be indistinguishable from
// genuine user traffic on the receiving end. Forcing every name that is not the
// local fixture server to fail resolution makes such a call die before it
// reaches the network stack.
//
// ~NOTFOUND, not an address: MAP to 0.0.0.0 would NOT be a dead end, because
// the kernel rewrites a connect() to the unspecified address as a connect() to
// loopback. That silently serves a foreign origin from the fixture server
// whenever the ports coincide, which is the opposite of containment.
//
// page.route() interception (scene3d_models / scene3d_quality) is unaffected —
// a fulfilled route never reaches the network stack.
const OFFLINE_RESOLVER_RULES = 'MAP * ~NOTFOUND, EXCLUDE 127.0.0.1, EXCLUDE localhost, EXCLUDE [::1]';

function launchOptions() {
  const options = { args: [`--host-resolver-rules=${OFFLINE_RESOLVER_RULES}`] };
  if (process.env.PJ_CHROMIUM_EXECUTABLE) {
    options.executablePath = process.env.PJ_CHROMIUM_EXECUTABLE;
  }
  return options;
}

module.exports = { launchOptions, OFFLINE_RESOLVER_RULES };
