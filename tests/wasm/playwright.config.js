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
    'scene3d_url_models.spec.js',
    'presets.spec.js',
    'scene3d_quality.spec.js',
    'editors.spec.js',
    'streaming.spec.js',
    'bridge.spec.js',
    'robot_deployment.spec.js',
    'dialogs.spec.js',
    'python.spec.js',
    'python_layout.spec.js',
  ],
  // Every case boots its own page and context, so cases spread across workers and CI shards, file by
  // file or within one. Chromium: cases tagged @serial (time bounds, hover probes) fail on a loaded
  // runner, so they run alone afterwards (`npm run test`, then `npm run test:serial`). Firefox runs
  // this build several times slower, and its fixed-delay UI steps miss under load (4 of 46 cases
  // failed with three workers): one worker, with wasm-ci.yml sharding it across two jobs instead.
  fullyParallel: true,
  // Three on CI's 4-core runners; one core stays for the static server and the browsers' own threads.
  workers: process.env.CI ? 3 : undefined,
  // No project name in snapshot paths: both browsers compare against one
  // committed baseline set.
  snapshotPathTemplate: '{snapshotDir}/{testFileDir}/{testFileName}-snapshots/{arg}{-snapshotSuffix}{ext}',
  use: {
    viewport: { width: 1280, height: 720 },
    screenshot: 'only-on-failure',
  },
  projects: browserProjects(),
});
