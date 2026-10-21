// SPDX-License-Identifier: MPL-2.0
const { defineConfig } = require('@playwright/test');
const { browserProjects } = require('./support/browser');

module.exports = defineConfig({
  // The one list of functional specs, shared by both browser projects.
  testMatch: [
    'ingress_file_selection.spec.js',
    'persistence_layout.spec.js',
    'plotting.spec.js',
    'scene2d.spec.js',
    'scene3d_foundation.spec.js',
    'scene3d_data.spec.js',
    'scene3d_models.spec.js',
    'scene3d_quality.spec.js',
    'editors.spec.js',
    'streaming.spec.js',
    'dialogs.spec.js',
    'python.spec.js',
    'python_layout.spec.js',
  ],
  // No project name in snapshot paths: both browsers compare against one
  // committed baseline set.
  snapshotPathTemplate: '{snapshotDir}/{testFileDir}/{testFileName}-snapshots/{arg}{-snapshotSuffix}{ext}',
  use: {
    viewport: { width: 1280, height: 720 },
    screenshot: 'only-on-failure',
  },
  projects: browserProjects(),
});
