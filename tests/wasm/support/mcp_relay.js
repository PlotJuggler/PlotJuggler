// SPDX-License-Identifier: MPL-2.0
const http = require('node:http');

// A stand-in for the agent relay (pj_mcp_relay.py, in the plotjuggler-agent
// repository), speaking the page protocol of McpBrowserLink
// (docs/MCP_SERVER.md, "Browser relay"): the page says hello, long-polls for
// the agent's JSON-RPC messages and posts its answers back. It also serves the
// files open_file hands to the page. linkUrl goes in the page's ?mcp= query.
async function startMcpRelay() {
  const key = 'wasm-spec-relay-key';
  const queue = [];          // messages no poll has taken yet
  const polls = [];          // parked polls: send(item | null)
  const answers = new Map(); // seq -> resolve
  const files = new Map();   // name -> Buffer
  let seq = 0;
  let joined;
  const pageJoined = new Promise((resolve) => { joined = resolve; });

  const send = (res, req, status, body) => {
    const data = body === undefined ? Buffer.alloc(0) : Buffer.isBuffer(body) ? body : Buffer.from(JSON.stringify(body));
    const headers = { 'Content-Length': data.length };
    if (req.headers.origin) {
      headers['Access-Control-Allow-Origin'] = req.headers.origin;
      headers.Vary = 'Origin';
    }
    if (body !== undefined) {
      headers['Content-Type'] = Buffer.isBuffer(body) ? 'application/octet-stream' : 'application/json';
    }
    res.writeHead(status, headers);
    res.end(data);
  };

  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://127.0.0.1');
    if (req.method === 'OPTIONS') {  // the CORS preflight the X-PJ-Link header causes
      res.writeHead(204, {
        'Access-Control-Allow-Origin': req.headers.origin || '*',
        'Access-Control-Allow-Methods': 'GET, POST',
        'Access-Control-Allow-Headers': 'Content-Type, X-PJ-Link',
        'Access-Control-Allow-Private-Network': 'true',
        'Content-Length': 0,
      });
      res.end();
      return;
    }
    if (req.method === 'GET' && url.pathname.startsWith('/file/')) {
      const file = files.get(decodeURIComponent(url.pathname.slice('/file/'.length)));
      send(res, req, file ? 200 : 404, file);
      return;
    }
    if (req.headers['x-pj-link'] !== key) {
      send(res, req, 403);
      return;
    }
    if (req.method === 'GET' && url.pathname === '/link/next') {
      if (queue.length > 0) {
        send(res, req, 200, queue.shift());
        return;
      }
      const timer = setTimeout(() => answer(null), 20000);
      const leave = () => {
        clearTimeout(timer);
        if (polls.includes(answer)) polls.splice(polls.indexOf(answer), 1);
      };
      const answer = (item) => {
        leave();
        item ? send(res, req, 200, item) : send(res, req, 204);
      };
      polls.push(answer);
      res.on('close', leave);  // a poll the page dropped must not swallow the next message
      return;
    }
    let text = '';
    req.on('data', (chunk) => { text += chunk; });
    req.on('end', () => {
      const body = JSON.parse(text || 'null');
      if (url.pathname === '/link/hello') {
        send(res, req, 200, { id: 'page' });
        joined();
      } else if (url.pathname === '/link/reply') {
        const resolve = answers.get(body.seq);
        answers.delete(body.seq);
        if (resolve) resolve(body.response);
        send(res, req, 204);
      } else {
        send(res, req, 404);
      }
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const origin = `http://127.0.0.1:${server.address().port}`;

  // Sends one JSON-RPC request to the page and resolves with its response.
  const request = (method, params = {}) => new Promise((resolve) => {
    seq += 1;
    answers.set(seq, resolve);
    const item = { seq, client: 'Browser spec', message: { jsonrpc: '2.0', id: seq, method, params } };
    polls.length > 0 ? polls[0](item) : queue.push(item);
  });

  return {
    linkUrl: `${origin}/?key=${key}`,
    pageJoined,
    request,
    // One tools/call: { value, isError }, the text parsed as JSON when it is.
    async call(name, args = {}) {
      const response = await request('tools/call', { name, arguments: args });
      const result = response.result || {};
      const text = (result.content || []).filter(part => part.type === 'text').map(part => part.text).join('\n');
      let value = text;
      try { value = JSON.parse(text); } catch { /* plain text */ }
      return { value, isError: Boolean(result.isError), error: response.error };
    },
    // Serves `bytes` as the file `name` (again replaces it) and returns the url open_file takes.
    serveFile(name, bytes) {
      files.set(name, Buffer.from(bytes));
      return `${origin}/file/${encodeURIComponent(name)}`;
    },
    close: () => new Promise((resolve) => {
      for (const answer of [...polls]) answer(null);
      server.closeAllConnections();
      server.close(resolve);
    }),
  };
}

module.exports = { startMcpRelay };
