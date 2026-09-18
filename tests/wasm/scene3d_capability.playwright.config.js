// SPDX-License-Identifier: MPL-2.0
const { defineConfig } = require('@playwright/test');
const { launchOptions } = require('./support/browser');

module.exports = defineConfig({
  testMatch: 'scene3d_capability.spec.js',
  timeout: 120_000,
  expect: {
    timeout: 90_000,
  },
  fullyParallel: false,
  workers: 1,
  reporter: 'line',
  use: {
    viewport: { width: 1280, height: 760 },
    screenshot: 'only-on-failure',
  },
  projects: [
    {
      name: 'chromium',
      use: {
        browserName: 'chromium',
        launchOptions: launchOptions(),
      },
    },
  ],
});
