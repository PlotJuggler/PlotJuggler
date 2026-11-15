#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""A minimal Foxglove WebSocket server for the browser tests.

Speaks the part of the Foxglove protocol the official plugin uses
(pj-official-plugins data_stream_foxglove_bridge): it selects the first subprotocol the client
offers that it knows, sends serverInfo and one advertise of JSON channels on open, applies
subscribe and unsubscribe, and every 50 ms sends a message-data frame (opcode 0x01, subscription id,
log time ns, JSON payload) per subscription.

Listens on a free 127.0.0.1 port and reports on stdout as bridge_server.py does: "listening", then
per connection "connect" (path and offered subprotocols), "open" (the selected one), "request"
(command and topic names, so the bridge helper's subscribed() works unchanged) and "closed".
"""

import asyncio
import base64
import hashlib
import json
import math
import struct
import time

from bridge_server import OPCODE_BINARY, OPCODE_CLOSE, OPCODE_PING, OPCODE_PONG, OPCODE_TEXT, WEBSOCKET_GUID
from bridge_server import read_ws_frame, report, ws_frame

SUBPROTOCOLS = ("foxglove.sdk.v1", "foxglove.websocket.v1")
# Self-describing JSON channels, decoded by the JSON parser plugin.
CHANNELS = {channel_id: {"id": channel_id, "topic": topic, "encoding": "json", "schemaName": "Float64",
                         "schema": "", "schemaEncoding": "jsonschema"}
            for channel_id, topic in ((1, "/robot/counter"), (2, "/robot/sine"), (3, "/robot/unused"))}


class Session:
    def __init__(self, writer):
        self.writer = writer
        self.subscriptions = {}  # subscription id -> channel id

    def send_json(self, message):
        self.writer.write(ws_frame(OPCODE_TEXT, json.dumps(message).encode()))

    def on_message(self, message):
        op = message.get("op")
        if op == "subscribe":
            fresh = {entry["id"]: entry["channelId"] for entry in message.get("subscriptions", [])
                     if entry.get("channelId") in CHANNELS}
            self.subscriptions.update(fresh)
            report("request", command="subscribe", topics=[CHANNELS[ch]["topic"] for ch in fresh.values()])
        elif op == "unsubscribe":
            removed = [self.subscriptions.pop(sub) for sub in message.get("subscriptionIds", [])
                       if sub in self.subscriptions]
            report("request", command="unsubscribe", topics=[CHANNELS[ch]["topic"] for ch in removed])

    async def stream(self):
        frame_number = 0
        while True:
            await asyncio.sleep(0.05)
            stamp = time.time_ns()
            values = {"/robot/counter": frame_number, "/robot/sine": math.sin(frame_number * 0.1)}
            for subscription_id, channel_id in list(self.subscriptions.items()):
                payload = json.dumps({"value": values.get(CHANNELS[channel_id]["topic"], 0.0)}).encode()
                self.writer.write(ws_frame(OPCODE_BINARY, struct.pack("<BIQ", 1, subscription_id, stamp) + payload))
            frame_number += 1
            await self.writer.drain()


async def handle(reader, writer):
    try:
        request = (await reader.readuntil(b"\r\n\r\n")).decode("latin-1").split("\r\n")
    except (asyncio.IncompleteReadError, ConnectionError):
        writer.close()
        return
    path = request[0].split(" ")[1]
    headers = {name.strip().lower(): value.strip()
               for name, _, value in (line.partition(":") for line in request[1:] if line)}
    offered = [name.strip() for name in headers.get("sec-websocket-protocol", "").split(",") if name.strip()]
    report("connect", path=path, subprotocols=offered)
    selected = next((name for name in offered if name in SUBPROTOCOLS), None)
    try:
        accept = base64.b64encode(hashlib.sha1(headers["sec-websocket-key"].encode() + WEBSOCKET_GUID).digest())
        response = (b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    b"Sec-WebSocket-Accept: " + accept + b"\r\n")
        if selected:
            response += b"Sec-WebSocket-Protocol: " + selected.encode() + b"\r\n"
        writer.write(response + b"\r\n")
        report("open", path=path, subprotocol=selected)
        session = Session(writer)
        session.send_json({"op": "serverInfo", "name": "pj4_browser_test_foxglove", "capabilities": []})
        session.send_json({"op": "advertise", "channels": list(CHANNELS.values())})
        streamer = asyncio.create_task(session.stream())
        try:
            while True:
                opcode, payload = await read_ws_frame(reader)
                if opcode == OPCODE_TEXT:
                    session.on_message(json.loads(payload))
                elif opcode == OPCODE_PING:
                    writer.write(ws_frame(OPCODE_PONG, payload))
                elif opcode == OPCODE_CLOSE:
                    writer.write(ws_frame(OPCODE_CLOSE, payload[:2]))
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
    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    report("listening", port=server.sockets[0].getsockname()[1])
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
