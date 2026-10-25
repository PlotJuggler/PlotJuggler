#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""A minimal PlotJuggler Bridge server for the browser tests.

Speaks the demand-mode control plane the official plugin uses (get_topics, subscribe_topic_updates,
additive subscribe and unsubscribe, heartbeat, pause and resume) and sends one zstd-compressed PJRB
data frame of JSON payloads every 50 ms for the subscribed topics. The wire format is the plugin's
(pj-official-plugins data_stream_pj_bridge/pj_bridge_protocol.hpp and test_scripts/).

    bridge_server.py [--hold-handshake]

Listens on a free 127.0.0.1 port and reports on stdout, one JSON object per line:
{"event": "listening", "port"}, then per connection "connect" (with its path), "open",
"request" (command and topics) and "closed". --hold-handshake reads the upgrade request and never
answers it, so the client stays connecting until it gives up.

zstd comes from the standard library on Python 3.14+, from the `zstandard` package before that.
"""

import argparse
import asyncio
import base64
import hashlib
import json
import math
import struct
import time

try:
    from compression.zstd import compress as zstd_compress
except ImportError:
    import zstandard

    def zstd_compress(data):
        return zstandard.ZstdCompressor().compress(data)

# Values are JSON, decoded by the JSON parser plugin. /robot/counter carries the frame number.
TOPICS = {name: {"name": name, "type": "Float64", "encoding": "json", "definition": ""}
          for name in ("/robot/counter", "/robot/sine", "/robot/unused")}
PJRB_MAGIC = 0x42524A50
WEBSOCKET_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OPCODE_TEXT = 1
OPCODE_BINARY = 2
OPCODE_CLOSE = 8
OPCODE_PING = 9
OPCODE_PONG = 10


def report(event, **fields):
    print(json.dumps({"event": event, **fields}), flush=True)


def data_frame(messages):
    """[magic][count][uncompressed size][flags=0][zstd payload]; each payload message is
    [topic length:2][topic][timestamp ns:8][data length:4][data], all little-endian."""
    payload = b"".join(struct.pack("<H", len(topic)) + topic + struct.pack("<qI", stamp, len(data)) + data
                       for topic, stamp, data in messages)
    return struct.pack("<IIII", PJRB_MAGIC, len(messages), len(payload), 0) + zstd_compress(payload)


def ws_frame(opcode, payload):
    size = len(payload)
    if size < 126:
        header = struct.pack("!BB", 0x80 | opcode, size)
    elif size < 1 << 16:
        header = struct.pack("!BBH", 0x80 | opcode, 126, size)
    else:
        header = struct.pack("!BBQ", 0x80 | opcode, 127, size)
    return header + payload


async def read_ws_frame(reader):
    """One client frame as (opcode, payload). Client frames are masked; the plugin's control frames
    are small, so continuation frames are not reassembled."""
    first, second = await reader.readexactly(2)
    size = second & 0x7F
    if size == 126:
        (size,) = struct.unpack("!H", await reader.readexactly(2))
    elif size == 127:
        (size,) = struct.unpack("!Q", await reader.readexactly(8))
    mask = await reader.readexactly(4) if second & 0x80 else b"\0\0\0\0"
    data = await reader.readexactly(size)
    return first & 0x0F, bytes(byte ^ mask[index % 4] for index, byte in enumerate(data))


class Session:
    def __init__(self, writer):
        self.writer = writer
        self.subscribed = []
        self.paused = False

    def send(self, opcode, payload):
        self.writer.write(ws_frame(opcode, payload))

    def reply(self, request, response):
        response["protocol_version"] = 1
        if isinstance(request.get("id"), str):
            response["id"] = request["id"]
        self.send(OPCODE_TEXT, json.dumps(response).encode())

    def on_request(self, request):
        command = request.get("command")
        names = [name for name in request.get("topics", []) if isinstance(name, str)]
        report("request", command=command, topics=names)
        if command == "get_topics":
            fields = ("name", "type", "encoding", "definition") if request.get("include_schemas") else ("name", "type")
            self.reply(request, {
                "status": "success",
                "server": {"name": "pj4_browser_test_bridge", "version": "1",
                           "capabilities": ["include_schemas", "topics_changed"]},
                "topics": [{key: topic[key] for key in fields} for topic in TOPICS.values()]})
        elif command == "subscribe":
            fresh = [name for name in names if name in TOPICS and name not in self.subscribed]
            self.subscribed += fresh
            failures = [{"topic": name, "reason": "Topic does not exist"} for name in names if name not in TOPICS]
            self.reply(request, {
                "status": "partial_success" if failures else "success",
                "schemas": {name: {"encoding": "json", "definition": ""} for name in fresh},
                "failures": failures})
        elif command == "unsubscribe":
            self.subscribed = [name for name in self.subscribed if name not in names]
            self.reply(request, {"status": "success", "removed": names})
        elif command in ("pause", "resume"):
            self.paused = command == "pause"
            self.reply(request, {"status": "ok", "paused": self.paused})
        elif command in ("heartbeat", "subscribe_topic_updates"):
            self.reply(request, {"status": "ok"})

    async def stream(self):
        frame_number = 0
        while True:
            await asyncio.sleep(0.05)
            if self.subscribed and not self.paused:
                stamp = time.time_ns()
                values = {"/robot/counter": frame_number, "/robot/sine": math.sin(frame_number * 0.1)}
                self.send(OPCODE_BINARY, data_frame([
                    (name.encode(), stamp, json.dumps({"value": values.get(name, 0.0)}).encode())
                    for name in self.subscribed]))
                frame_number += 1
            await self.writer.drain()


async def handle(reader, writer, hold_handshake):
    try:
        request = (await reader.readuntil(b"\r\n\r\n")).decode("latin-1").split("\r\n")
    except (asyncio.IncompleteReadError, ConnectionError):
        writer.close()  # a connection that sent no request is no client
        return
    path = request[0].split(" ")[1]
    headers = {name.strip().lower(): value.strip()
               for name, _, value in (line.partition(":") for line in request[1:] if line)}
    report("connect", path=path)
    try:
        if hold_handshake:
            await reader.read()  # returns once the client drops the connection
            return
        accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + WEBSOCKET_GUID).digest())
        writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        report("open", path=path)
        session = Session(writer)
        streamer = asyncio.create_task(session.stream())
        try:
            while True:
                opcode, payload = await read_ws_frame(reader)
                if opcode == OPCODE_TEXT:
                    session.on_request(json.loads(payload))
                elif opcode == OPCODE_PING:
                    session.send(OPCODE_PONG, payload)
                elif opcode == OPCODE_CLOSE:
                    session.send(OPCODE_CLOSE, payload[:2])
                    await writer.drain()
                    return
        finally:
            streamer.cancel()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        report("closed", path=path)
        writer.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--hold-handshake", action="store_true")
    options = parser.parse_args()
    server = await asyncio.start_server(
        lambda reader, writer: handle(reader, writer, options.hold_handshake), "127.0.0.1", 0)
    report("listening", port=server.sockets[0].getsockname()[1])
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
