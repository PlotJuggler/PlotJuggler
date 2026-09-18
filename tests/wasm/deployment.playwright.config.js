// SPDX-License-Identifier: MPL-2.0
const { defineConfig } = require('@playwright/test');
const { launchOptions } = require('./support/browser');

module.exports = defineConfig({
  testMatch: 'deployment.spec.js',
  timeout: 180_000,
  expect: {
    timeout: 90_000,
  },
  fullyParallel: false,
  workers: 1,
  reporter: 'line',
  use: {
    viewport: { width: 1280, height: 720 },
    screenshot: 'only-on-failure',
    launchOptions: launchOptions(),
  },
});
