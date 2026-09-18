// SPDX-License-Identifier: MPL-2.0
const { defineConfig } = require('@playwright/test');
const { launchOptions } = require('./support/browser');

module.exports = defineConfig({
  use: {
    viewport: { width: 1280, height: 720 },
    screenshot: 'only-on-failure',
    launchOptions: launchOptions(),
  },
});
