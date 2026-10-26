// SPDX-License-Identifier: MPL-2.0
const http = require('http');

// A real second local origin (another port), so cross-origin model fetches go
// through the browser's CORS checks under the page's COEP. `routes` maps a
// path to { body, cors = true, hold, status = 200 }: `cors: false` omits
// Access-Control-Allow-Origin, and `hold` delays the response until release().
// Unknown paths answer 404 (with CORS, so the page sees the status). Every
// request is logged as "METHOD /path".
async function startSecondOrigin(routes) {
  const requests = [];
  const held = [];
  const server = http.createServer((request, response) => {
    const path = new URL(request.url, 'http://second.origin').pathname;
    requests.push(`${request.method} ${path}`);
    const route = routes[path];
    const headers = route && route.cors === false ? {} : { 'access-control-allow-origin': '*' };
    response.on('error', () => {});
    if (request.method === 'OPTIONS') {
      response.writeHead(204, {
        ...headers,
        'access-control-allow-headers': '*',
        'access-control-allow-methods': 'GET',
      });
      response.end();
    } else if (!route) {
      response.writeHead(404, headers);
      response.end();
    } else {
      const send = () => {
        response.writeHead(route.status || 200, { ...headers, 'cache-control': 'no-store' });
        response.end(route.body);
      };
      if (route.hold) {
        held.push(send);
      } else {
        send();
      }
    }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  return {
    origin: `http://127.0.0.1:${server.address().port}`,
    requests,
    release: () => held.splice(0).forEach(send => send()),
    close: () => new Promise((resolve) => {
      server.closeAllConnections();
      server.close(resolve);
    }),
  };
}

module.exports = { startSecondOrigin };
