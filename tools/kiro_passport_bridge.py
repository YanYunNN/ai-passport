#!/usr/bin/env python3
"""
Kiro AI Passport Bridge CLI and Hook Handler.

This script bridges Kiro IDE/CLI hooks and local developers with the Cloudflare
Passport Relay (https://ws.yanyun.asia) to deliver hardware-in-the-loop approvals
for high-risk AI agent operations.
"""

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any, Dict, Optional, Tuple
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_RELAY_URL = "https://ws.yanyun.asia"
DEFAULT_CONFIG_PATHS = [
    Path.cwd() / ".kiro" / "passport_config.json",
    Path.home() / ".kiro" / "passport_config.json",
    Path.cwd() / ".kiro_passport.json",
]

TOOL_PATTERN = re.compile(r"^[A-Za-z0-9._:-]{1,31}$")
DEVICE_ID_PATTERN = re.compile(r"^passport-[A-F0-9]{12}$")


def load_config() -> Dict[str, Any]:
    """Load configuration from files and environment variables."""
    config: Dict[str, Any] = {
        "relay_url": os.environ.get("KIRO_PASSPORT_RELAY_URL")
        or os.environ.get("PASSPORT_RELAY_URL")
        or DEFAULT_RELAY_URL,
        "device_id": os.environ.get("KIRO_PASSPORT_DEVICE_ID")
        or os.environ.get("PASSPORT_DEVICE_ID"),
        "hook_token": os.environ.get("KIRO_PASSPORT_HOOK_TOKEN")
        or os.environ.get("HOOK_AUTH_SECRET"),
        "admin_key": os.environ.get("KIRO_PASSPORT_ADMIN_KEY")
        or os.environ.get("ADMIN_API_KEY"),
    }

    for path in DEFAULT_CONFIG_PATHS:
        if path.is_file():
            try:
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    if isinstance(data, dict):
                        for k in ["relay_url", "device_id", "hook_token", "admin_key"]:
                            if data.get(k) and not config.get(k):
                                config[k] = data[k]
            except Exception as e:
                print(f"[Warning] Failed to read config file {path}: {e}", file=sys.stderr)

    return config


def save_config(config_data: Dict[str, Any], global_config: bool = False) -> Path:
    """Save configuration to local or global config file."""
    if global_config:
        target_path = Path.home() / ".kiro" / "passport_config.json"
    else:
        target_path = Path.cwd() / ".kiro" / "passport_config.json"

    target_path.parent.mkdir(parents=True, exist_ok=True)

    existing = {}
    if target_path.is_file():
        try:
            with open(target_path, "r", encoding="utf-8") as f:
                existing = json.load(f)
        except Exception:
            existing = {}

    existing.update({k: v for k, v in config_data.items() if v is not None})

    with open(target_path, "w", encoding="utf-8") as f:
        json.dump(existing, f, indent=2, ensure_ascii=False)

    return target_path


def sanitize_tool(raw_tool: str) -> str:
    """Sanitize tool name to match 1..31 alphanumeric/._:- characters."""
    cleaned = re.sub(r"[^A-Za-z0-9._:-]", "_", (raw_tool or "tool").strip())
    if not cleaned:
        cleaned = "tool"
    return cleaned[:31]


def sanitize_summary(raw_summary: str) -> str:
    """
    Sanitize summary string according to ESP32 firmware constraints:
    1..71 printable ASCII characters, without double quotes or backslashes.
    """
    if not raw_summary:
        raw_summary = "High-risk tool execution"

    # Replace forbidden chars: quotes, backslash, non-printable, non-ASCII
    cleaned = ""
    for char in raw_summary:
        code = ord(char)
        if 0x20 <= code <= 0x7E and char not in ('"', '\\'):
            cleaned += char
        elif char in ('"', "'", '`'):
            cleaned += "'"
        elif char == '\\':
            cleaned += "/"
        elif code > 0x7E:
            # Transliterate or space
            cleaned += " "
        elif char in ('\r', '\n', '\t'):
            cleaned += " "

    # Collapse multiple spaces and strip
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    if not cleaned:
        cleaned = "Operation request"

    return cleaned[:71]


def extract_hook_payload() -> Tuple[str, str]:
    """Extract tool name and summary from Kiro hook stdin payload."""
    tool_name = "execute_bash"
    summary = "High-risk tool call"

    try:
        if not sys.stdin.isatty():
            stdin_text = sys.stdin.read().strip()
            if stdin_text:
                try:
                    payload = json.loads(stdin_text)
                    if isinstance(payload, dict):
                        # Detect tool name from various hook schemas
                        tool_name = (
                            payload.get("tool_name")
                            or payload.get("tool")
                            or payload.get("name")
                            or payload.get("trigger_matcher")
                            or tool_name
                        )

                        # Detect summary from tool arguments / inputs
                        args = (
                            payload.get("tool_input")
                            or payload.get("arguments")
                            or payload.get("parameters")
                            or payload.get("input")
                            or {}
                        )

                        if isinstance(args, dict):
                            if "command" in args:
                                summary = f"cmd: {args['command']}"
                            elif "path" in args or "file_path" in args:
                                path = args.get("path") or args.get("file_path")
                                summary = f"{tool_name} {path}"
                            elif "query" in args:
                                summary = f"query: {args['query']}"
                            else:
                                summary = f"{tool_name}: {json.dumps(args, ensure_ascii=False)}"
                        elif isinstance(args, str):
                            summary = f"{tool_name}: {args}"
                        else:
                            summary = f"{tool_name} requested"
                except json.JSONDecodeError:
                    summary = stdin_text[:71]
    except Exception as e:
        print(f"[Warning] Failed reading hook stdin: {e}", file=sys.stderr)

    return sanitize_tool(tool_name), sanitize_summary(summary)


def http_request(
    url: str,
    method: str = "GET",
    headers: Optional[Dict[str, str]] = None,
    data: Optional[Dict[str, Any]] = None,
    timeout: float = 10.0,
) -> Tuple[int, Dict[str, Any]]:
    """Execute JSON HTTP request."""
    req_headers = {
        "Accept": "application/json",
        "User-Agent": "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 KiroPassportBridge/1.0",
    }
    if headers:
        req_headers.update(headers)

    body_bytes = None
    if data is not None:
        req_headers["Content-Type"] = "application/json; charset=utf-8"
        body_bytes = json.dumps(data).encode("utf-8")

    req = urllib.request.Request(url, data=body_bytes, headers=req_headers, method=method)

    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = resp.status
            resp_body = resp.read().decode("utf-8")
            try:
                parsed = json.loads(resp_body) if resp_body else {}
            except json.JSONDecodeError:
                parsed = {"raw": resp_body}
            return status, parsed
    except urllib.error.HTTPError as e:
        resp_body = e.read().decode("utf-8") if e.fp else ""
        try:
            parsed = json.loads(resp_body) if resp_body else {}
        except json.JSONDecodeError:
            parsed = {"raw": resp_body}
        return e.code, parsed
    except Exception as e:
        return 0, {"error": str(e)}


def request_approval(
    relay_url: str,
    device_id: str,
    hook_token: str,
    tool: str,
    summary: str,
    ttl_seconds: int = 30,
) -> Tuple[bool, str, Dict[str, Any]]:
    """
    Submit approval request to Cloudflare Relay and poll for decision.
    Returns (allowed: bool, reason: str, details: dict).
    """
    relay_url = relay_url.rstrip("/")
    if not hook_token:
        return False, "missing_hook_token", {"error": "HOOK_AUTH_SECRET / hook_token is not configured"}

    if not device_id or not DEVICE_ID_PATTERN.match(device_id):
        return False, "invalid_device_id", {"error": f"Invalid or missing device_id: {device_id}"}

    headers = {"Authorization": f"Bearer {hook_token}"}
    create_url = f"{relay_url}/v1/devices/{device_id}/requests"
    payload = {
        "tool": sanitize_tool(tool),
        "summary": sanitize_summary(summary),
        "ttl_seconds": max(5, min(300, ttl_seconds)),
    }

    print(
        f"[Passport Bridge] Submitting approval to {device_id} via {relay_url} (Tool: {payload['tool']}, TTL: {payload['ttl_seconds']}s)...",
        file=sys.stderr,
    )
    print(f"[Passport Bridge] Summary: \"{payload['summary']}\"", file=sys.stderr)

    status_code, resp = http_request(create_url, method="POST", headers=headers, data=payload, timeout=10.0)

    if status_code not in (200, 202):
        err = resp.get("error") or f"HTTP {status_code}"
        print(f"[Passport Bridge] Failed to create approval request: {err}", file=sys.stderr)
        return False, f"create_failed_{err}", resp

    request_id = resp.get("request_id")
    if not request_id:
        return False, "missing_request_id", resp

    if resp.get("status") in ("allow", "deny"):
        status = resp.get("status")
        reason = resp.get("reason", "direct")
        return (status == "allow"), reason, resp

    # Polling loop
    poll_url = f"{relay_url}/v1/requests/{request_id}"
    deadline = time.monotonic() + payload["ttl_seconds"] + 5.0
    poll_interval = 0.6

    print(f"[Passport Bridge] Request ID: {request_id}. Awaiting user button click on AI Passport...", file=sys.stderr)

    while time.monotonic() < deadline:
        time.sleep(poll_interval)
        poll_status, poll_resp = http_request(poll_url, method="GET", headers=headers, timeout=8.0)

        if poll_status == 200:
            current_status = poll_resp.get("status")
            reason = poll_resp.get("reason", "unknown")

            if current_status == "allow":
                print(f"[Passport Bridge] ✅ APPROVED by user on device ({reason})", file=sys.stderr)
                return True, reason, poll_resp
            elif current_status == "deny":
                print(f"[Passport Bridge] ❌ DENIED by device ({reason})", file=sys.stderr)
                return False, reason, poll_resp
            elif current_status == "pending":
                # Still waiting for user interaction
                continue
            else:
                print(f"[Passport Bridge] Unexpected status: {current_status}", file=sys.stderr)
                return False, f"unexpected_{current_status}", poll_resp
        elif poll_status == 404:
            print("[Passport Bridge] Request not found or expired", file=sys.stderr)
            return False, "not_found", poll_resp
        else:
            # Temporary network error during poll, continue trying until deadline
            continue

    print("[Passport Bridge] ⏱️ Timeout waiting for approval decision", file=sys.stderr)
    return False, "timeout", {"status": "deny", "reason": "timeout"}


def cmd_hook(args: argparse.Namespace, config: Dict[str, Any]) -> int:
    """Handle hook invocation from Kiro."""
    kind = args.kind.lower()

    # Informational hooks
    if kind in ("busy", "idle"):
        # Informational state updates are non-blocking
        return 0

    if kind != "pretool":
        print(f"[Passport Bridge] Unhandled hook kind: {kind}", file=sys.stderr)
        return 0

    # PreToolUse high-risk approval check
    tool, summary = extract_hook_payload()
    if args.tool:
        tool = sanitize_tool(args.tool)
    if args.summary:
        summary = sanitize_summary(args.summary)

    relay_url = args.relay_url or config.get("relay_url") or DEFAULT_RELAY_URL
    device_id = args.device_id or config.get("device_id")
    hook_token = args.token or config.get("hook_token")
    timeout = args.timeout or 30

    if not device_id:
        print("[Passport Bridge] ⚠️ Error: No device_id configured. Set KIRO_PASSPORT_DEVICE_ID or run: python3 tools/kiro_passport_bridge.py config --device-id <ID>", file=sys.stderr)
        return 1

    if not hook_token:
        print("[Passport Bridge] ⚠️ Error: No hook_token configured. Set HOOK_AUTH_SECRET or run: python3 tools/kiro_passport_bridge.py config --token <SECRET>", file=sys.stderr)
        return 1

    allowed, reason, details = request_approval(
        relay_url=relay_url,
        device_id=device_id,
        hook_token=hook_token,
        tool=tool,
        summary=summary,
        ttl_seconds=timeout,
    )

    if allowed:
        print(f"[Passport Hook] Tool '{tool}' APPROVED.")
        return 0
    else:
        print(f"[Passport Hook] Tool '{tool}' DENIED ({reason}).", file=sys.stderr)
        return 1


def cmd_request(args: argparse.Namespace, config: Dict[str, Any]) -> int:
    """Manually test an approval request."""
    relay_url = args.relay_url or config.get("relay_url") or DEFAULT_RELAY_URL
    device_id = args.device_id or config.get("device_id")
    hook_token = args.token or config.get("hook_token")
    tool = sanitize_tool(args.tool or "execute_bash")
    summary = sanitize_summary(args.summary or "Test approval request")
    timeout = args.timeout or 30

    if not device_id:
        print("Error: device_id is required. Provide --device-id or configure one.", file=sys.stderr)
        return 1

    if not hook_token:
        print("Error: hook_token is required. Provide --token or configure one.", file=sys.stderr)
        return 1

    allowed, reason, details = request_approval(
        relay_url=relay_url,
        device_id=device_id,
        hook_token=hook_token,
        tool=tool,
        summary=summary,
        ttl_seconds=timeout,
    )

    print(json.dumps({"allowed": allowed, "reason": reason, "details": details}, indent=2))
    return 0 if allowed else 1


def cmd_config(args: argparse.Namespace, config: Dict[str, Any]) -> int:
    """View or update local/global configuration."""
    updates = {}
    if args.device_id:
        if not DEVICE_ID_PATTERN.match(args.device_id):
            print(f"Error: Invalid device_id format: {args.device_id} (Expected passport-XXXXXXXXXXXX)", file=sys.stderr)
            return 1
        updates["device_id"] = args.device_id
    if args.token:
        updates["hook_token"] = args.token
    if args.relay_url:
        updates["relay_url"] = args.relay_url.rstrip("/")
    if args.admin_key:
        updates["admin_key"] = args.admin_key

    if updates:
        target_path = save_config(updates, global_config=args.global_config)
        print(f"✅ Configuration saved to: {target_path}")

    current = load_config()
    # Mask token for display
    display_config = dict(current)
    if display_config.get("hook_token"):
        token = display_config["hook_token"]
        display_config["hook_token"] = token[:4] + "..." + token[-4:] if len(token) > 8 else "***"
    if display_config.get("admin_key"):
        key = display_config["admin_key"]
        display_config["admin_key"] = key[:4] + "..." + key[-4:] if len(key) > 8 else "***"

    print("\nCurrent Configuration:")
    print(json.dumps(display_config, indent=2))
    return 0


def cmd_status(args: argparse.Namespace, config: Dict[str, Any]) -> int:
    """Check relay server health and device status."""
    relay_url = (args.relay_url or config.get("relay_url") or DEFAULT_RELAY_URL).rstrip("/")
    device_id = args.device_id or config.get("device_id")

    print(f"Checking Relay: {relay_url} ...")
    health_status, health_resp = http_request(f"{relay_url}/healthz", timeout=5.0)
    if health_status == 200 and health_resp.get("ok"):
        print("  ✅ Cloudflare Relay Worker is healthy and reachable.")
    else:
        print(f"  ❌ Relay unreachable or unhealthy: HTTP {health_status} {health_resp}")
        return 1

    if device_id:
        print(f"\nTarget Device: {device_id}")
    else:
        print("\nTarget Device: (None configured)")

    admin_key = config.get("admin_key")
    if admin_key:
        print("Querying device status via Admin API...")
        headers = {"Authorization": f"Bearer {admin_key}"}
        status_code, resp = http_request(f"{relay_url}/v1/admin/devices", headers=headers, timeout=5.0)
        if status_code == 200:
            devices = resp.get("devices", [])
            print(f"Found {len(devices)} registered device(s):")
            for d in devices:
                online_str = "🟢 Online" if d.get("online") else "⚪ Offline"
                status_str = "Active" if d.get("status") == "active" else "Revoked"
                is_target = " [TARGET]" if d.get("device_id") == device_id else ""
                print(f"  - {d.get('device_id')}: {online_str} ({status_str}){is_target}")
        else:
            print(f"Admin query returned HTTP {status_code}: {resp}")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Kiro AI Passport Bridge CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # hook command
    hook_parser = subparsers.add_parser("hook", help="Execute Kiro hook trigger")
    hook_parser.add_argument("--kind", choices=["pretool", "busy", "idle"], default="pretool", help="Hook kind")
    hook_parser.add_argument("--tool", help="Override tool name")
    hook_parser.add_argument("--summary", help="Override summary text")
    hook_parser.add_argument("--timeout", type=int, default=30, help="Approval timeout in seconds")
    hook_parser.add_argument("--device-id", help="Target device ID")
    hook_parser.add_argument("--token", help="Hook auth secret token")
    hook_parser.add_argument("--relay-url", help="Relay server URL")

    # request command
    req_parser = subparsers.add_parser("request", help="Manually submit test approval request")
    req_parser.add_argument("--tool", default="execute_bash", help="Tool name")
    req_parser.add_argument("--summary", default="Test passport approval", help="Summary text")
    req_parser.add_argument("--timeout", type=int, default=30, help="Timeout in seconds")
    req_parser.add_argument("--device-id", help="Target device ID")
    req_parser.add_argument("--token", help="Hook auth secret token")
    req_parser.add_argument("--relay-url", help="Relay server URL")

    # config command
    config_parser = subparsers.add_parser("config", help="View or update bridge configuration")
    config_parser.add_argument("--device-id", help="Set default device ID")
    config_parser.add_argument("--token", help="Set HOOK_AUTH_SECRET token")
    config_parser.add_argument("--relay-url", help="Set Relay URL")
    config_parser.add_argument("--admin-key", help="Set Admin API key")
    config_parser.add_argument("--global", dest="global_config", action="store_true", help="Save to ~/.kiro/passport_config.json")

    # status command
    status_parser = subparsers.add_parser("status", help="Check relay health and devices")
    status_parser.add_argument("--device-id", help="Device ID to check")
    status_parser.add_argument("--relay-url", help="Relay server URL")

    args = parser.parse_args()
    config = load_config()

    if args.command == "hook":
        return cmd_hook(args, config)
    elif args.command == "request":
        return cmd_request(args, config)
    elif args.command == "config":
        return cmd_config(args, config)
    elif args.command == "status":
        return cmd_status(args, config)
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())
