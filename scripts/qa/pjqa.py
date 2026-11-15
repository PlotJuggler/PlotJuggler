#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Drive an isolated PlotJuggler 4 for QA, through its MCP server's tools (the UI tools included).

  pjqa.py [--home DIR] start [--xvfb] [--plugin-dir DIR] [-- APP_ARGS...]
  pjqa.py call TOOL ['{"json": "args"}']     prints the result text; exit 1 when the tool failed
  pjqa.py run SCRIPT                         replays a script (see below); exit 1 on the first failure
  pjqa.py stop

--home (or $PJQA_HOME) selects the instance; give every command the same one.

The app runs from this checkout's run.sh with --mcp-ui-tools, offscreen (or under xvfb-run), under a
private HOME, so it never touches the user's settings, windows or running instances. Linux only.

A script has one step per line: `TOOL {json args}`, `expect TEXT` (the previous result must contain
TEXT), `refuse TOOL {json}` (the call must fail), or a `#` comment. Name widgets by objectName there,
since refs change from one run to the next.
"""

import argparse
import base64
import json
import os
import shlex
import signal
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_HOME = Path(tempfile.gettempdir()) / f"pjqa-{os.getuid()}"
STATE = "pjqa.json"


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
        command = ["xvfb-run", "-a", "-s", "-screen 0 1920x1080x24"] + command
        env["QT_QPA_PLATFORM"] = "xcb"  # a Wayland session would otherwise put the app on the user's desktop
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
                return
            time.sleep(0.2)
        sys.exit(f"pjqa: no MCP server within 60 s; see {home / 'app.log'}")
    except BaseException:  # a timeout, an interrupt or an error: no state file names the app, so end it here
        terminate(app)
        raise


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
    request = urllib.request.Request(
        f"http://127.0.0.1:{state['port']}/mcp",
        data=body,
        headers={"Authorization": f"Bearer {state['token']}", "X-PJ-Client": "qa", "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=120) as reply:
        answer = json.loads(reply.read())
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


def run(args):
    state = state_of(args)
    last = ""
    for number, line in enumerate(Path(args.script).read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        word, _, rest = line.partition(" ")
        if word == "expect":
            ok = rest in last
        else:
            refuse = word == "refuse"
            tool, _, raw = (rest if refuse else line).partition(" ")
            last, failed = call_tool(state, tool, parse_args_json(raw))
            ok = failed == refuse
        print(f"{'ok  ' if ok else 'FAIL'} {number}: {line}")
        if not ok:
            print(last)
            sys.exit(1)
    print("pjqa: script passed")


def stop(args):
    home = home_dir(args)
    path = home / STATE
    if not path.exists():
        print("pjqa: nothing to stop")
        return
    state = json.loads(path.read_text())
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
    p.add_argument("--xvfb", action="store_true", help="a virtual X display instead of offscreen")
    p.add_argument("--plugin-dir", help="a folder of built plugins (data loaders, parsers)")
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
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
