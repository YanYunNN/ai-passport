#!/usr/bin/env python3
"""Local macOS BLE bridge for the Kiro Passport GATT service.

Install once in a virtual environment:
  python3 -m venv .venv-kiro-passport
  .venv-kiro-passport/bin/pip install -r tools/requirements-kiro-passport.txt

Run the bridge before using Kiro hooks:
  .venv-kiro-passport/bin/python tools/kiro_passport_bridge.py serve

The hook subcommand is intentionally stdlib-only except when `serve` imports
Bleak. It communicates only over a user-local Unix socket and sends no tool
arguments or source content to the device.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
from pathlib import Path
import socket
import stat
import sys
import uuid
from collections.abc import Awaitable, Callable
from typing import Any

SERVICE_UUID = "3eaa0001-5a2c-4f4a-8d17-7f8d73c11901"
COMMAND_UUID = "3eaa0002-5a2c-4f4a-8d17-7f8d73c11901"
STATUS_UUID = "3eaa0003-5a2c-4f4a-8d17-7f8d73c11901"
DEFAULT_SOCKET = Path("/tmp/kiro-passport.sock")
MAX_MESSAGE_BYTES = 224


def compact_text(value: Any, maximum: int) -> str:
    """Return protocol-safe printable text without exposing tool inputs."""
    text = str(value).replace("\\", "_").replace('"', "_")
    text = " ".join(text.split())
    return "".join(char if 32 <= ord(char) < 127 else "?" for char in text)[:maximum]


class PassportBridge:
    def __init__(self, device_address: str | None) -> None:
        self.device_address = device_address
        self.client: Any | None = None
        self.pending: dict[str, asyncio.Future[str]] = {}
        self.disconnected = asyncio.Event()
        self.write_lock = asyncio.Lock()

    def on_disconnect(self, _client: Any) -> None:
        self.disconnected.set()
        self.fail_pending("device disconnected")

    def fail_pending(self, reason: str) -> None:
        for future in self.pending.values():
            if not future.done():
                future.set_exception(ConnectionError(reason))

    @staticmethod
    def service_matches(_device: Any, advertisement: Any) -> bool:
        return SERVICE_UUID in {uuid.lower() for uuid in advertisement.service_uuids}

    async def find_device(self) -> Any | None:
        from bleak import BleakScanner

        if self.device_address:
            return await BleakScanner.find_device_by_address(self.device_address, timeout=8.0)
        return await BleakScanner.find_device_by_filter(self.service_matches, timeout=8.0)

    async def run(self) -> None:
        from bleak import BleakClient

        while True:
            device = await self.find_device()
            if device is None:
                await asyncio.sleep(2.0)
                continue
            self.disconnected.clear()
            try:
                async with BleakClient(device, disconnected_callback=self.on_disconnect) as client:
                    self.client = client
                    await client.start_notify(STATUS_UUID, self.on_status)
                    await self.send({"v": 1, "type": "state", "state": "idle"})
                    await self.disconnected.wait()
            except Exception as error:  # Bluetooth and pairing failures must not allow a tool.
                print(f"Kiro Passport BLE: {error}", file=sys.stderr)
                self.fail_pending("Bluetooth unavailable")
                await asyncio.sleep(2.0)
            finally:
                self.client = None
                self.disconnected.set()

    def on_status(self, _sender: int, data: bytearray) -> None:
        try:
            message = json.loads(bytes(data).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if message.get("type") != "status":
            return
        request_id = message.get("id")
        decision = message.get("decision")
        if isinstance(request_id, str) and decision in {"allow", "deny"}:
            future = self.pending.get(request_id)
            if future is not None and not future.done():
                future.set_result(decision)

    async def send(self, message: dict[str, object]) -> None:
        if self.client is None or not self.client.is_connected:
            raise ConnectionError("Kiro Passport is not connected")
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        if len(payload) >= MAX_MESSAGE_BYTES:
            raise ValueError("Passport protocol message is too large")
        async with self.write_lock:
            # The encrypted GATT characteristic asks macOS to pair automatically on first access.
            await self.client.write_gatt_char(COMMAND_UUID, payload, response=True)

    async def request(self, tool: str, timeout: float) -> dict[str, object]:
        request_id = str(uuid.uuid4())
        decision_future: asyncio.Future[str] = asyncio.get_running_loop().create_future()
        self.pending[request_id] = decision_future
        try:
            await self.send(
                {
                    "v": 1,
                    "type": "request",
                    "id": request_id,
                    "tool": compact_text(tool, 31) or "Kiro tool",
                    "summary": "Kiro requests permission",
                }
            )
            decision = await asyncio.wait_for(decision_future, timeout=timeout)
            return {"allow": decision == "allow", "reason": f"Passport {decision}"}
        except (ConnectionError, TimeoutError, ValueError, asyncio.TimeoutError) as error:
            return {"allow": False, "reason": str(error) or "Passport approval timed out"}
        finally:
            self.pending.pop(request_id, None)
            try:
                await self.send({"v": 1, "type": "clear", "id": request_id})
            except (ConnectionError, ValueError):
                pass

    async def handle(self, message: dict[str, object]) -> dict[str, object]:
        action = message.get("action")
        if action == "status":
            state = message.get("state")
            if state not in {"idle", "busy", "sleep", "error"}:
                return {"ok": False, "reason": "invalid status"}
            try:
                await self.send({"v": 1, "type": "state", "state": state})
            except (ConnectionError, ValueError) as error:
                return {"ok": False, "reason": str(error)}
            return {"ok": True}
        if action == "request":
            tool = message.get("tool")
            timeout = message.get("timeout", 30)
            if not isinstance(tool, str) or not isinstance(timeout, (int, float)):
                return {"allow": False, "reason": "invalid approval request"}
            return await self.request(tool, max(1.0, min(float(timeout), 60.0)))
        return {"ok": False, "reason": "unknown action"}


async def serve_request(bridge: PassportBridge, reader: asyncio.StreamReader,
                        writer: asyncio.StreamWriter) -> None:
    try:
        raw = await asyncio.wait_for(reader.readline(), timeout=2.0)
        if not raw or len(raw) > 1024:
            response: dict[str, object] = {"ok": False, "reason": "invalid local request"}
        else:
            try:
                message = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                message = None
            response = await bridge.handle(message) if isinstance(message, dict) else {
                "ok": False, "reason": "invalid JSON"
            }
        writer.write(json.dumps(response, separators=(",", ":")).encode("utf-8") + b"\n")
        await writer.drain()
    finally:
        writer.close()
        await writer.wait_closed()


def prepare_socket(path: Path) -> None:
    if not path.exists():
        return
    mode = path.lstat().st_mode
    if not stat.S_ISSOCK(mode):
        raise RuntimeError(f"refusing to replace non-socket path: {path}")
    path.unlink()


async def serve(socket_path: Path, device_address: str | None) -> None:
    bridge = PassportBridge(device_address)
    prepare_socket(socket_path)
    server = await asyncio.start_unix_server(
        lambda reader, writer: serve_request(bridge, reader, writer), path=str(socket_path)
    )
    os.chmod(socket_path, 0o600)
    bridge_task = asyncio.create_task(bridge.run())
    print(f"Kiro Passport bridge listening on {socket_path}")
    try:
        async with server:
            await server.serve_forever()
    finally:
        bridge_task.cancel()
        await asyncio.gather(bridge_task, return_exceptions=True)
        if socket_path.exists() and stat.S_ISSOCK(socket_path.lstat().st_mode):
            socket_path.unlink()


def local_request(socket_path: Path, payload: dict[str, object]) -> dict[str, object]:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(2.0)
        client.connect(str(socket_path))
        client.sendall(json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n")
        response = bytearray()
        while not response.endswith(b"\n"):
            chunk = client.recv(1024)
            if not chunk:
                break
            response.extend(chunk)
    return json.loads(response.decode("utf-8"))


def event_tool(event: dict[str, Any]) -> str:
    tool = event.get("tool_name") or event.get("name") or event.get("tool")
    if isinstance(tool, dict):
        tool = tool.get("name") or tool.get("tool_name")
    return compact_text(tool or "Kiro tool", 31)


def run_hook(kind: str, timeout: float, socket_path: Path) -> int:
    try:
        event = json.load(sys.stdin)
    except json.JSONDecodeError:
        event = {}

    if kind == "pretool":
        try:
            response = local_request(socket_path, {
                "action": "request", "tool": event_tool(event), "timeout": timeout,
            })
        except (OSError, TimeoutError, ValueError, json.JSONDecodeError) as error:
            print(f"Passport denied tool: bridge unavailable ({error})", file=sys.stderr)
            return 2
        if response.get("allow") is True:
            return 0
        print(f"Passport denied tool: {response.get('reason', 'not approved')}", file=sys.stderr)
        return 2

    state = "busy" if kind == "busy" else "idle"
    try:
        local_request(socket_path, {"action": "status", "state": state})
    except (OSError, TimeoutError, ValueError, json.JSONDecodeError):
        # Status synchronization must never block ordinary conversational work.
        pass
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Kiro Passport local BLE bridge")
    subcommands = parser.add_subparsers(dest="command", required=True)

    serve_parser = subcommands.add_parser("serve", help="run the BLE bridge and local socket")
    serve_parser.add_argument("--socket", type=Path, default=DEFAULT_SOCKET)
    serve_parser.add_argument("--device", default=os.environ.get("KIRO_PASSPORT_DEVICE"),
                              help="optional BLE address or CoreBluetooth identifier")

    hook_parser = subcommands.add_parser("hook", help="called by Kiro Hooks")
    hook_parser.add_argument("--kind", choices=("busy", "idle", "pretool"), required=True)
    hook_parser.add_argument("--timeout", type=float, default=30.0)
    hook_parser.add_argument("--socket", type=Path, default=DEFAULT_SOCKET)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.command == "serve":
        try:
            asyncio.run(serve(args.socket, args.device))
        except KeyboardInterrupt:
            return 0
        return 0
    return run_hook(args.kind, args.timeout, args.socket)


if __name__ == "__main__":
    raise SystemExit(main())
