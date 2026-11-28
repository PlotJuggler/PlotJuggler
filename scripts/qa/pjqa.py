#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Drive an isolated PlotJuggler 4 for QA, through its MCP server's tools (the UI tools included).

  pjqa.py [--home DIR] start [--xvfb [WxH] | --display :N|wayland] [--plugin-dir DIR] [-- APP_ARGS...]
  pjqa.py [--home DIR] start --browser DEPLOY_DIR   the browser build: prints the page URL to open
  pjqa.py call TOOL ['{"json": "args"}']     prints the result text; exit 1 when the tool failed
  pjqa.py run SCRIPT                         replays a script (see below); exit 1 on the first failure
  pjqa.py stop
  pjqa.py regress [--plugin-dir DIR] [SCRIPT_OR_DIR...]
                                             each script in a fresh instance (default: scripts/qa/regressions/);
                                             a summary, exit 1 when any failed

--home (or $PJQA_HOME) selects the instance; give every command the same one.

The app runs from this checkout's run.sh with --mcp-ui-tools, offscreen (or under xvfb-run), under a
private HOME, so it never touches the user's settings, windows or running instances. Linux only.
--display puts its windows on that display instead: an X display `:N` (a private GPU display, say), or
`wayland` for the session's own compositor; a user's own display only when they asked. `-geometry`
among the app arguments works on every platform; a Wayland compositor places the window itself.
Offscreen has no QRhi, so plots still draw but 2D/3D scenes and image views draw nothing; `start` says so on
stderr. --xvfb (default screen 1920x1080, or e.g. --xvfb 2560x1440) renders them in software.

--browser serves a packaged WebAssembly build (scripts/package_wasm.mjs output) and runs a stand-in for
the agent relay (docs/MCP_SERVER.md, "Browser relay"); `call` and `run` then reach the page that opens the
printed URL (with Playwright). The browser build has the data tools, not the UI tools: click there with
the browser's own input. open_file takes a local path, which the relay serves to the page.

A script has one step per line: `TOOL {json args}`, `expect TEXT` (the previous result must contain
TEXT), `absent TEXT` (it must not), `refuse TOOL {json}` (the call must fail), `until TOOL {json} TEXT`
(repeat the call until it succeeds with TEXT in its result, at most UNTIL_S seconds: for what the app does
after a call returns, such as a load at startup), `grows TOOL {json} KEY` (repeat the call until the number
at the top-level KEY of its JSON result exceeds the first call's, at most UNTIL_S seconds: a cursor that
moves), or a `#` comment. Name widgets by objectName there, since
refs change from one run to the next.

For `regress`, a script names how its instance starts in header comments: `# app: ARGS` (the app
arguments, paths relative to the checkout) and `# needs: plugins` when it loads data through a plugin
(skipped without --plugin-dir). In a step, `$CHECKOUT` stands for the checkout's absolute path (open_file
takes only absolute paths). A failed script keeps its instance's home (app.log, screenshots).
"""

import argparse
import base64
import json
import os
import re
import secrets
import shlex
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HOME = Path(tempfile.gettempdir()) / f"pjqa-{os.getuid()}"
STATE = "pjqa.json"
UNTIL_S = 30


def home_dir(args):
    return Path(args.home or os.environ.get("PJQA_HOME") or DEFAULT_HOME)


def private_home(args):
    """The home, created owner-only: it holds the MCP bearer token."""
    home = home_dir(args)
    home.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = home.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        sys.exit(f"pjqa: {home} is not a directory owned by you")
    home.chmod(0o700)
    return home


def birth(pid):
    """The process's start time (clock ticks since boot), or None when no such process: with the pid it names one
    process, so a recorded pid that the kernel has reused is never signalled."""
    try:
        return int(Path(f"/proc/{pid}/stat").read_text().rpartition(")")[2].split()[19])
    except (OSError, ValueError, IndexError):
        return None


def terminate(app):
    """Ends a launched app that was never handed over to `stop`."""
    try:
        os.killpg(app.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        app.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(app.pid, signal.SIGKILL)
        app.wait()


def start(args):
    home = private_home(args)
    if (home / STATE).exists():
        sys.exit(f"pjqa: an instance is recorded in {home}; run `pjqa.py stop` first")
    if args.browser:
        start_browser(args, home)
        return
    env = {
        **os.environ,
        "HOME": str(home),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "PJ_DISABLE_TELEMETRY": "1",
    }
    command = [str(ROOT / "run.sh"), "--nosplash", "--mcp-ui-tools"]
    if args.plugin_dir:
        command += ["--plugin-dir", str(Path(args.plugin_dir).resolve())]
    command += args.app_args
    if args.xvfb:
        command = ["xvfb-run", "-a", "-s", f"-screen 0 {args.xvfb}x24"] + command
        env["QT_QPA_PLATFORM"] = "xcb"  # a Wayland session would otherwise put the app on the user's desktop
        env.pop("WAYLAND_DISPLAY", None)
    elif args.display == "wayland":
        env["QT_QPA_PLATFORM"] = "wayland"
    elif args.display:
        env["QT_QPA_PLATFORM"] = "xcb"
        env["DISPLAY"] = args.display
        env.pop("WAYLAND_DISPLAY", None)
    else:
        env["QT_QPA_PLATFORM"] = "offscreen"
    instances = home / ".local/share/PlotJuggler/PlotJuggler4/mcp-instances"
    for stale in instances.glob("*.json") if instances.is_dir() else []:
        stale.unlink()  # left by an instance that was killed; the new one writes its own
    with open(home / "app.log", "wb") as log:
        app = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        deadline = time.time() + 60
        while time.time() < deadline:
            if app.poll() is not None:
                sys.exit(f"pjqa: the app exited ({app.returncode}); see {home / 'app.log'}")
            for entry in instances.glob("*.json") if instances.is_dir() else []:
                found = json.loads(entry.read_text())
                state = {
                    "group": [app.pid, birth(app.pid)],
                    "pid": [found["pid"], birth(found["pid"])],
                    "port": found["port"],
                    "token": found["token"],
                }
                fd = os.open(home / STATE, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "w") as out:
                    json.dump(state, out)
                print(f"pjqa: ready (pid {found['pid']}, home {home})")
                if env["QT_QPA_PLATFORM"] == "offscreen":
                    print(
                        "pjqa: offscreen has no QRhi: 2D/3D scenes and image views draw nothing (blank in "
                        "screenshots); start with --xvfb to see them",
                        file=sys.stderr,
                    )
                return
            time.sleep(0.2)
        sys.exit(f"pjqa: no MCP server within 60 s; see {home / 'app.log'}")
    except BaseException:  # a timeout, an interrupt or an error: no state file names the app, so end it here
        terminate(app)
        raise


def screen_size(value):
    if re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*", value):
        return value
    raise argparse.ArgumentTypeError("expected WIDTHxHEIGHT, e.g. 2560x1440")


def display_value(value):
    if value == "wayland" or (value.startswith(":") and value[1:].replace(".", "", 1).isdigit()):
        return value
    raise argparse.ArgumentTypeError("expected :N (an X display) or wayland")


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def start_browser(args, home):
    """The package's server and the relay, each in its own process group; `stop` ends both."""
    root = Path(args.browser).resolve()
    if not (root / "manifest.json").is_file():
        sys.exit(f"pjqa: {root} is not a packaged browser build (no manifest.json); see docs/WASM_DEPLOYMENT.md")
    serve_port, relay_port, key = free_port(), free_port(), secrets.token_urlsafe(24)
    commands = [
        ("serve", ["node", str(ROOT / "scripts/serve_wasm.mjs"), "--root", str(root), "--port", str(serve_port)]),
        ("relay", [sys.executable, str(Path(__file__).resolve()), "--home", str(home), "relay", str(relay_port)]),
    ]
    children = []
    try:
        for name, command in commands:
            with open(home / f"{name}.log", "wb") as log:
                children.append(
                    subprocess.Popen(
                        command,
                        env={**os.environ, "PJQA_RELAY_KEY": key},
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        start_new_session=True,
                    )
                )
        for port in (serve_port, relay_port):
            deadline = time.time() + 20
            while True:
                if any(child.poll() is not None for child in children):
                    sys.exit(f"pjqa: a helper exited; see {home / 'serve.log'} and {home / 'relay.log'}")
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=1).close()
                    break
                except OSError:
                    if time.time() > deadline:
                        sys.exit(f"pjqa: nothing answers on port {port} within 20 s")
                    time.sleep(0.2)
        relay = f"http://127.0.0.1:{relay_port}/?key={key}"
        url = f"http://127.0.0.1:{serve_port}/?mcp={urllib.parse.quote(relay, safe='')}"
        state = {"browser": True, "groups": [[c.pid, birth(c.pid)] for c in children], "port": relay_port,
                 "token": key, "url": url}
        fd = os.open(home / STATE, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as out:
            json.dump(state, out)
    except BaseException:
        for child in children:
            terminate(child)
        raise
    print(f"pjqa: open {url}")


def relay(args):
    """A stand-in for the agent relay (plotjuggler-agent's pj_mcp_relay.py), speaking the page protocol of
    McpBrowserLink, with POST /call for `pjqa.py call` instead of MCP over stdio."""
    key = os.environ["PJQA_RELAY_KEY"]
    lock = threading.Condition()
    queue, answers, files = [], {}, {}
    sequence = [0]

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def send(self, status, body=None, kind="application/json"):
            data = b"" if body is None else body if isinstance(body, bytes) else json.dumps(body).encode()
            self.send_response(status)
            if self.headers.get("Origin"):
                self.send_header("Access-Control-Allow-Origin", self.headers["Origin"])
                self.send_header("Vary", "Origin")
            if body is not None:
                self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_OPTIONS(self):  # the CORS preflight the X-PJ-Link header causes
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", self.headers.get("Origin") or "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, X-PJ-Link")
            self.send_header("Access-Control-Allow-Private-Network", "true")
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path.startswith("/file/"):  # the token in the path is the secret, as with the real relay
                found = files.get(path.split("/")[2])
                return self.send(200, found.read_bytes(), "application/octet-stream") if found else self.send(404)
            if self.headers.get("X-PJ-Link") != key or path != "/link/next":
                return self.send(403 if path == "/link/next" else 404)
            with lock:
                lock.wait_for(lambda: queue, timeout=20)
                item = queue.pop(0) if queue else None
            if item is None:
                return self.send(204)
            try:
                self.send(200, item)
            except OSError:  # the page dropped this poll: the next one takes the message
                with lock:
                    queue.insert(0, item)
                    lock.notify_all()

        def do_POST(self):
            if self.headers.get("X-PJ-Link") != key:
                return self.send(403)
            path = urllib.parse.urlsplit(self.path).path
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"null")
            if path == "/link/hello":
                return self.send(200, {"id": "page"})
            if path == "/link/reply":
                with lock:
                    if body.get("seq") in answers:
                        answers[body["seq"]] = body.get("response")
                        lock.notify_all()
                return self.send(204)
            if path != "/call":
                return self.send(404)
            params = body.get("params", {})
            arguments = params.get("arguments", {})
            if params.get("name") == "open_file" and isinstance(arguments.get("path"), str):
                local = Path(arguments["path"]).expanduser().resolve()
                token = secrets.token_urlsafe(16)
                files[token] = local
                arguments["url"] = f"http://127.0.0.1:{self.server.server_port}/file/{token}/" + urllib.parse.quote(
                    local.name
                )
                arguments["name"] = local.name
            with lock:
                sequence[0] += 1
                seq = sequence[0]
                answers[seq] = None
                queue.append({"seq": seq, "client": "qa", "message": {**body, "id": seq}})
                lock.notify_all()
                lock.wait_for(lambda: answers[seq] is not None, timeout=300)
                response = answers.pop(seq)
            if response is None:
                return self.send(504, {"error": {"message": "no page answered in 300 s: is the printed URL open?"}})
            self.send(200, response)

    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


def state_of(args):
    path = home_dir(args) / STATE
    if not path.exists():
        sys.exit("pjqa: no instance; run `pjqa.py start` first")
    return {**json.loads(path.read_text()), "home": str(home_dir(args))}


def call_tool(state, tool, arguments):
    """One tools/call; returns (text, failed)."""
    body = json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": tool, "arguments": arguments}}
    ).encode()
    if state.get("browser"):
        url, headers = f"http://127.0.0.1:{state['port']}/call", {"X-PJ-Link": state["token"]}
    else:
        url, headers = f"http://127.0.0.1:{state['port']}/mcp", {"Authorization": f"Bearer {state['token']}"}
    request = urllib.request.Request(
        url, data=body, headers={**headers, "X-PJ-Client": "qa", "Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=320) as reply:
            answer = json.loads(reply.read())
    except urllib.error.HTTPError as error:
        answer = json.loads(error.read() or b"{}") or {"error": {"message": str(error)}}
    if "error" in answer:
        return answer["error"].get("message", json.dumps(answer["error"])), True
    result = answer.get("result", {})
    lines = []
    for content in result.get("content", []):
        if content.get("type") == "text":
            lines.append(content.get("text", ""))
        elif content.get("type") == "image":
            # An image is saved next to the instance, for the agent to open by its path.
            shot = Path(state["home"]) / f"shot-{time.strftime('%H%M%S')}-{time.time_ns() % 10**9:09d}.png"
            shot.write_bytes(base64.b64decode(content.get("data", "")))
            lines.append(f"image saved to {shot}")
    return "\n".join(lines), bool(result.get("isError"))


def parse_args_json(text):
    try:
        return json.loads(text) if text and text.strip() else {}
    except json.JSONDecodeError as error:
        sys.exit(f"pjqa: the arguments are not JSON ({error}): {text}")


def call(args):
    text, failed = call_tool(state_of(args), args.tool, parse_args_json(args.json))
    print(text)
    sys.exit(1 if failed else 0)


def replay(state, script):
    """Runs the script's steps against the instance; returns whether every step held."""
    last = ""
    for number, line in enumerate(Path(script).read_text().splitlines(), 1):
        line = line.strip().replace("$CHECKOUT", json.dumps(str(ROOT))[1:-1])  # tools that take only absolute paths
        if not line or line.startswith("#"):
            continue
        word, _, rest = line.partition(" ")
        if word == "expect":
            ok = rest in last
        elif word == "absent":
            ok = rest not in last
        elif word == "until":
            tool, _, raw = rest.partition(" ")
            arguments, end = json.JSONDecoder().raw_decode(raw.strip())
            text = raw.strip()[end:].strip()
            deadline = time.time() + UNTIL_S
            while True:
                last, failed = call_tool(state, tool, arguments)
                ok = not failed and text in last
                if ok or time.time() > deadline:
                    break
                time.sleep(0.2)
        elif word == "grows":
            tool, _, raw = rest.partition(" ")
            arguments, end = json.JSONDecoder().raw_decode(raw.strip())
            key = raw.strip()[end:].strip()
            deadline = time.time() + UNTIL_S
            first = None
            while True:
                last, failed = call_tool(state, tool, arguments)
                try:
                    value = None if failed else json.loads(last)[key]
                except (json.JSONDecodeError, KeyError, TypeError):
                    value = None
                first = value if first is None else first
                ok = value is not None and value > first
                if ok or time.time() > deadline:
                    break
                time.sleep(0.2)
        else:
            refuse = word == "refuse"
            tool, _, raw = (rest if refuse else line).partition(" ")
            last, failed = call_tool(state, tool, parse_args_json(raw))
            ok = failed == refuse
        print(f"{'ok  ' if ok else 'FAIL'} {number}: {line}")
        if not ok:
            print(last)
            return False
    return True


def run(args):
    if not replay(state_of(args), args.script):
        sys.exit(1)
    print("pjqa: script passed")


def header(script, key):
    """The value of a `# key: value` header comment, or None."""
    for line in Path(script).read_text().splitlines():
        if line.startswith(f"# {key}:"):
            return line.partition(":")[2].strip()
    return None


def regress(args):
    scripts = []
    for target in args.targets or [ROOT / "scripts/qa/regressions"]:
        target = Path(target).resolve()
        scripts += sorted(target.glob("*.pjqa")) if target.is_dir() else [target]
    if not scripts:
        sys.exit("pjqa: no scripts to run")
    plugin_dir = str(Path(args.plugin_dir).resolve()) if args.plugin_dir else None
    os.chdir(ROOT)  # `# app:` paths are relative to the checkout
    results = []
    for script in scripts:
        home = Path(tempfile.mkdtemp(prefix=f"pjqa-{script.stem}-"))
        instance = argparse.Namespace(home=str(home), browser=None, xvfb=None, display=None, plugin_dir=plugin_dir, app_args=[])
        print(f"== {script.name}")
        try:
            if header(script, "needs") == "plugins" and not plugin_dir:
                shutil.rmtree(home, ignore_errors=True)
                results.append((script, "SKIP", "needs --plugin-dir"))
                continue
            instance.app_args = shlex.split(header(script, "app") or "")
            start(instance)
            passed = replay(state_of(instance), script)
        except (SystemExit, Exception) as error:  # a failed launch, a crashed app, a bad script: this script fails
            print(f"pjqa: {error!r}")
            passed = False
        finally:
            stop(instance)
        if passed:
            shutil.rmtree(home, ignore_errors=True)
        results.append((script, "PASS" if passed else "FAIL", "" if passed else f"kept {home}"))
    print()
    for script, verdict, note in results:
        print(f"{verdict}  {script.name}  {note}".rstrip())
    if any(verdict == "FAIL" for _, verdict, _ in results):
        sys.exit(1)


def stop(args):
    home = home_dir(args)
    path = home / STATE
    if not path.exists():
        print("pjqa: nothing to stop")
        return
    state = json.loads(path.read_text())
    for group, group_birth in state.get("groups", []):  # --browser: the server and the relay
        if group_birth is not None and birth(group) == group_birth:
            os.killpg(group, signal.SIGTERM)
    if state.get("browser"):
        path.unlink()
        print("pjqa: stopped")
        return
    (app, app_birth), (group, group_birth) = state["pid"], state["group"]
    app_alive = app_birth is not None and birth(app) == app_birth
    # The group outlives its leader while the app is in it, so either one alive proves the group is still ours.
    targets = [(app, os.kill)] if app_alive else []
    if app_alive or (group_birth is not None and birth(group) == group_birth):
        targets.append((group, os.killpg))
    for pid, kill in targets:
        try:
            kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    path.unlink()
    print("pjqa: stopped")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--home", help=f"the instance's private HOME (default $PJQA_HOME or {DEFAULT_HOME})")
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("start")
    where = p.add_mutually_exclusive_group()
    where.add_argument(
        "--xvfb",
        nargs="?",
        const="1920x1080",
        type=screen_size,
        metavar="WxH",
        help="a virtual X display instead of offscreen, with this screen size (default 1920x1080)",
    )
    where.add_argument(
        "--display",
        type=display_value,
        metavar=":N|wayland",
        help="run on this X display, or natively on the session's Wayland compositor; windows appear there "
        "(a user's own display only when they asked)",
    )
    p.add_argument("--plugin-dir", help="a folder of built plugins (data loaders, parsers)")
    p.add_argument("--browser", metavar="DEPLOY_DIR", help="serve this packaged browser build instead")
    p.add_argument("app_args", nargs="*", help="after --: more app arguments, e.g. a file to open")
    p.set_defaults(func=start)
    p = commands.add_parser("call")
    p.add_argument("tool")
    p.add_argument("json", nargs="?")
    p.set_defaults(func=call)
    p = commands.add_parser("run")
    p.add_argument("script")
    p.set_defaults(func=run)
    commands.add_parser("stop").set_defaults(func=stop)
    p = commands.add_parser("regress")
    p.add_argument("--plugin-dir", help="a folder of built plugins, for scripts marked `# needs: plugins`")
    p.add_argument("targets", nargs="*", help="scripts or folders of scripts (default: scripts/qa/regressions)")
    p.set_defaults(func=regress)
    p = commands.add_parser("relay", help=argparse.SUPPRESS)  # started by `start --browser`
    p.add_argument("port", type=int)
    p.set_defaults(func=relay)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
