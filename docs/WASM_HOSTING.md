# Hosting PlotJuggler on a robot

A robot can serve the PlotJuggler browser app itself and stream to it from the
[PlotJuggler Bridge](https://github.com/PlotJuggler/plotjuggler_bridge) running
beside it. [Caddy](https://caddyserver.com/) terminates TLS, serves the
[deployment package](./WASM_DEPLOYMENT.md) with the headers the threaded app
needs, and forwards `/bridge` to the bridge. The app and its stream then share
one origin, so a preset whose stream URL is `${deployment.ws_origin}/bridge`
works on every robot unchanged. The configuration is
[`docs/wasm/Caddyfile`](./wasm/Caddyfile), set entirely through environment
variables. The bridge needs no HTTP server of its own.

```text
            https://robot.local/            ┌─ files ──► deployment package
browser ──────────────────────────► Caddy ──┤
            wss://robot.local/bridge        └─ ws://127.0.0.1:9090 ──► pj_bridge
```

## Why HTTPS

The app runs threads in WebAssembly. Browsers allow that only on a secure,
cross-origin isolated page: HTTPS (or `localhost`) with the
`Cross-Origin-Opener-Policy` and `Cross-Origin-Embedder-Policy` headers. Opened
over plain HTTP from the robot's name or address, the page does not start and
says so:

> PlotJuggler cannot start: this page was not served over HTTPS. The app must be
> served over HTTPS, or from localhost, with the headers
> Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy:
> require-corp.

An HTTPS page can open only `wss://` sockets, which is why the bridge is reached
through Caddy on the page's own origin rather than on its own port.

## Set up a robot

1. **Install Caddy** on the robot ([install guide](https://caddyserver.com/docs/install);
   one static binary, or the distribution package). CI tests the release pinned
   as `PJ_CADDY_VERSION` in [`versions.env`](../versions.env).
2. **Copy the deployment package** to the robot unchanged, for example to
   `/srv/plotjuggler`. Build it as in [`WASM_DEPLOYMENT.md`](./WASM_DEPLOYMENT.md),
   with the robot's presets: `--presets <dir>` (see [Robot presets](#robot-presets)).
3. **Copy the Caddyfile**, `docs/wasm/Caddyfile` from the PlotJuggler sources,
   to the robot, unchanged: for example to `/etc/caddy/Caddyfile`, the file the
   packaged Caddy service reads.
4. **Start the bridge** on its default port, 9090, where Caddy forwards `/bridge`
   (`pj_bridge_ros2`). On another port, set `PJ_BRIDGE_UPSTREAM=127.0.0.1:<port>`.
5. **Run Caddy** with the robot's settings:

   ```bash
   export PJ_ROBOT_HOST=robot.local        # the name or address clients open
   export PJ_WASM_ROOT=/srv/plotjuggler    # the unpacked package
   export PJ_TLS=internal                  # see Certificates
   caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile   # "Valid configuration"
   caddy run --config /etc/caddy/Caddyfile --adapter caddyfile
   ```

6. **Open** `https://robot.local/` on a client, or `https://robot.local/?preset=<id>`
   to open one preset directly.

| Variable | Default | Meaning |
|---|---|---|
| `PJ_ROBOT_HOST` | required | The name or address clients open, optionally with `:<port>` (443 otherwise). |
| `PJ_WASM_ROOT` | required | The deployment package directory. |
| `PJ_TLS` | `internal` | The arguments of Caddy's [`tls`](https://caddyserver.com/docs/caddyfile/directives/tls) directive; see [Certificates](#certificates). |
| `PJ_HTTP_PORT` | `80` | Plain-HTTP port; it only redirects to the HTTPS page. |
| `PJ_BRIDGE_UPSTREAM` | `127.0.0.1:9090` | Where `/bridge` is forwarded; 9090 is pj_bridge's default port. |

Ports 80 and 443 need privileges. Either run Caddy as the service its
distribution packages install (it reads `/etc/caddy/Caddyfile`; set the
variables with `systemctl edit caddy`, as `Environment=PJ_ROBOT_HOST=robot.local`
lines under `[Service]`), or allow the binary to bind them
(`sudo setcap cap_net_bind_service=+ep "$(command -v caddy)"`), or pick high
ports (`PJ_ROBOT_HOST=robot.local:8443 PJ_HTTP_PORT=8080`). If `PJ_ROBOT_HOST`
is unset, Caddy fails with "server block without any key is global
configuration".

## Certificates

The browser verifies the robot's certificate, and nothing in PlotJuggler can
switch that off. No preference, plugin setting or layout key skips
verification: in the browser, the bridge plugin's socket is the browser's own
WebSocket, and the browser alone decides which certificates to trust. The client
device must trust the certificate. Pick one of:

- **A public certificate (ACME)**, when the robot has a public DNS name that
  Let's Encrypt or ZeroSSL can reach for their challenges: set
  `PJ_ROBOT_HOST` to that name and `PJ_TLS` to an e-mail address
  (`PJ_TLS=ops@example.org`). Caddy obtains and renews the certificate, and
  every client already trusts it.
- **Caddy's local CA** (`PJ_TLS=internal`, the default), for a robot on a local
  network. Caddy creates a root CA on the robot and issues the robot's
  certificate from it. Install that root on each client device once: copy
  `<data dir>/caddy/pki/authorities/local/root.crt` from the robot. The data
  directory is `~/.local/share` of the user running Caddy, or
  `/var/lib/caddy/.local/share` for the packaged service. Then import it as a
  trusted root: in the operating system's certificate settings for Chrome,
  Edge and Safari on Windows, macOS, Android and iOS (on iOS, also enable full
  trust under Settings → General → About → Certificate Trust Settings). Firefox,
  and Chrome on Linux, keep their own store: import it under their certificate
  settings. Caddy keeps the root in its data directory: keep that directory,
  or every client has to import a new root.
- **Your own CA**: `PJ_TLS="/etc/pj/robot.crt /etc/pj/robot.key"`, a
  certificate whose subjectAltName names `PJ_ROBOT_HOST`, issued by a CA the
  clients already trust.

A client that does not trust the certificate gets the browser's own refusal
(Chromium: `NET::ERR_CERT_AUTHORITY_INVALID`) and never loads the app.

## Robot presets

A robot preset is an ordinary [stream preset](./WASM_PRESETS.md#streaming-sources)
whose bridge URL is the page's own origin:

```xml
<root binding="source" pj4_version="5">
  <!-- tabbed_widget with the views, as File → Save Layout with sources writes it -->
  <previouslyLoaded_StreamingSources>
    <streamInfo id="robot" source_name="Robot">
      <plugin ID="PlotJuggler Bridge" manifest_id="plotjuggler-bridge"><![CDATA[
        {"url":"${deployment.ws_origin}/bridge"}
      ]]></plugin>
    </streamInfo>
  </previouslyLoaded_StreamingSources>
</root>
```

Opened from `https://robot.local/` it connects to `wss://robot.local/bridge`;
from `https://10.0.0.7:8443/`, to `wss://10.0.0.7:8443/bridge`. List it in the
preset tree's `index.json` and package that tree with `--presets`:

```json
{"presets": [{"id": "live", "title": "Live robot", "layout": "live/layout.pj4.xml"}]}
```

`https://robot.local/?preset=live` then opens it with no dialog. A preset that
names a `ws://` URL instead fails once opened from the HTTPS page. The browser
refuses the socket before connecting (mixed content), the layout itself still
loads, and PlotJuggler shows a "Streaming" warning: "The stream failed: plugin:
failed to connect to PJ bridge at ws://…/bridge: the browser refused to open a
WebSocket to ws://…/bridge".

## What Caddy sends

The Caddyfile reproduces the package's [response contract](./WASM_DEPLOYMENT.md#required-http-contract):

- `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp`,
  `Cross-Origin-Resource-Policy: same-origin` and `X-Content-Type-Options: nosniff`
  on every response.
- The manifest's MIME types from a fixed table, a copy of `CONTENT_TYPES` in
  `scripts/package_wasm.mjs`, never the robot's MIME database. Extensions match
  in any case.
- `Cache-Control: public, max-age=31536000, immutable` below `assets/`
  (content-addressed), `no-cache` everywhere else.
- The `.br` and `.gz` siblings chosen by `Accept-Encoding`, with
  `Vary: Accept-Encoding`, below `presets/` as for the bundle. Caddy never
  compresses on the fly.
- 404 for `_headers` and for direct requests to the `.br`/`.gz` files.
- `Cache-Control: no-store` and a plain-text status on every error (a 404, or a
  502 when the bridge is down), below `assets/` too: a bundle file requested
  while the package is still being copied must load once it is there.
- A redirect from plain HTTP (`PJ_HTTP_PORT`) to the HTTPS page.

## Troubleshooting

| What you see | Cause | Fix |
|---|---|---|
| "PlotJuggler cannot start: this page was not served over HTTPS…" | The page was opened over `http://` from a name other than `localhost`. | Open `https://<robot>/`. |
| "PlotJuggler cannot start: this page is not cross-origin isolated…" | The server, or a proxy in front of it, does not send the COOP/COEP headers. | Serve through this Caddyfile, and keep the headers through any other proxy. |
| "PlotJuggler cannot start in this browser…" | The server sends the headers, but the browser cannot isolate the page (an Android WebView, as in DuckDuckGo). | Open the app in a current Chrome, Edge, Firefox or Safari. |
| A certificate warning instead of the app | The client does not trust the robot's certificate. | [Certificates](#certificates). |
| "The stream failed: … the browser refused to open a WebSocket to ws://…" | The preset names a `ws://` URL. | Use `${deployment.ws_origin}/bridge`, or a `wss://` URL. |
| "The stream failed: … failed to connect to PJ bridge at wss://<robot>/bridge…" | The bridge is not listening at `PJ_BRIDGE_UPSTREAM`: Caddy answers the upgrade with a 502. | Start the bridge on that port. |

The bridge itself still listens on every interface, unencrypted and without
authentication. Close its port to the network if every client should come
through Caddy; a desktop PlotJuggler on the same network otherwise keeps
connecting to it directly. Anyone who can reach the robot's HTTPS port can open
the app and its stream: this recipe adds no authentication.

Caddy's admin API listens on `localhost:2019`, so any process on the robot can
read and replace Caddy's configuration through it. It stays on because
`systemctl reload caddy` uses it. Caddy also serves HTTP/3 on UDP at the HTTPS
port, so a firewall rule for 443 covers both TCP and UDP; with UDP closed,
browsers use HTTP/2.

## Testing

`tests/wasm/robot_deployment.spec.js`, part of the Chromium functional suite,
runs this Caddyfile unmodified and configured only through its environment. It
serves the acceptance package (`PJ_WASM_ACCEPTANCE_PACKAGE_DIR`, by default
`build-wasm-probe/deploy`) in front of the fake bridge
(`tests/wasm/support/bridge_server.py`), with a throwaway CA. Chromium maps
`robot.test` to loopback and trusts exactly the test certificate. The spec
checks:

- that every manifest file, plus one file of every deployable type, is served
  as the package declares (the checks `deployment.spec.js` runs against the
  reference server, from `tests/wasm/support/deployment_contract.js`);
- an isolated boot, with a preset and its data downloaded through Caddy;
- a `${deployment.ws_origin}/bridge` preset streaming over `wss://` into its plots;
- the four failures above: plain HTTP from the robot's name, a page served
  without the isolation headers, an untrusted certificate, and a `ws://` URL.

It needs Caddy (`PJ_CADDY`, or `caddy` on `PATH`), `openssl` and the fake
bridge's Python. It does not use `PJ_WASM_URL`. Firefox skips it: only
Chromium can map a host name and trust one certificate from its command line.

```bash
PJ_CADDY=/path/to/caddy npm --prefix tests/wasm run test -- robot_deployment.spec.js
```
