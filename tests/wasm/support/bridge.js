// SPDX-License-Identifier: MPL-2.0
const { spawn } = require('child_process');
const path = require('path');
const readline = require('readline');
const { count } = require('./console');
const { requestMultiPlotState } = require('./plot_probes');
const { streamPreset } = require('./preset_server');

const BRIDGE_PLUGIN = '<plugin ID="PlotJuggler Bridge" manifest_id="plotjuggler-bridge">';

// Runs support/bridge_server.py (PJ_BRIDGE_PYTHON, default python3) on a free
// port. `events` collects what it reports: connect/open/request/closed.
async function startBridgeServer(args = []) {
  const server = spawn(process.env.PJ_BRIDGE_PYTHON || 'python3',
    [path.join(__dirname, 'bridge_server.py'), ...args], { stdio: ['ignore', 'pipe', 'inherit'] });
  const events = [];
  const port = await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.once('exit', code => reject(new Error(`bridge_server.py exited with ${code}`)));
    readline.createInterface({ input: server.stdout }).on('line', (line) => {
      const event = JSON.parse(line);
      events.push(event);
      if (event.event === 'listening') {
        resolve(event.port);
      }
    });
  });
  return {
    port,
    url: `ws://127.0.0.1:${port}/bridge`,
    events,
    seen: kind => events.filter(event => event.event === kind),
    // What the server streams now: subscribes add, unsubscribes remove.
    subscribed: () => {
      const topics = new Set();
      for (const { command, topics: names = [] } of events) {
        for (const name of names) {
          if (command === 'subscribe') {
            topics.add(name);
          } else if (command === 'unsubscribe') {
            topics.delete(name);
          }
        }
      }
      return [...topics].sort();
    },
    close: () => server.kill(),
  };
}

// A layout with one PlotJuggler Bridge stream (id "robot") connecting to `url`.
const robotPreset = (url, plots) => streamPreset(
  [{ id: 'robot', config: JSON.stringify({ url }), plugin: BRIDGE_PLUGIN }], plots);
const COUNTER_AND_SINE = [
  { id: 'plot_counter', curves: [['/robot/counter', 'value', 'robot']] },
  { id: 'plot_sine', curves: [['/robot/sine', 'value', 'robot']] },
];

// The plots that drew something, in layout order.
async function drawnPlots(page, messages) {
  const state = await requestMultiPlotState(page, messages, count(messages, 'PJ_WASM_MULTI_SUMMARY'));
  return state.plots.filter(plot => plot.vertices > 0);
}

module.exports = { COUNTER_AND_SINE, drawnPlots, robotPreset, startBridgeServer };
