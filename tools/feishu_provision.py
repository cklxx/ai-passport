#!/usr/bin/env python3
"""Provision an owner's Feishu application over physical USB serial."""

from __future__ import annotations

import argparse
import base64
import getpass
import json
import re
import sys
import time

PROTOCOL = "FAP-FEISHU/1"
APP_ID_PATTERN = re.compile(r"^cli_[A-Za-z0-9]+$")


def build_frame(app_id: str, app_secret: str) -> bytes:
    if not APP_ID_PATTERN.fullmatch(app_id) or len(app_id) >= 64:
        raise ValueError("App ID must be a Feishu cli_ application ID")
    if not app_secret or len(app_secret) >= 128:
        raise ValueError("App Secret is empty or too long")
    payload = json.dumps(
        {"app_id": app_id, "app_secret": app_secret},
        ensure_ascii=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return PROTOCOL.encode("ascii") + b" " + base64.b64encode(payload) + b"\n"


def choose_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("pyserial is required: python3 -m pip install pyserial") from exc
    ports = [item.device for item in list_ports.comports()]
    likely = [item for item in ports if "usb" in item.lower() or "acm" in item.lower()]
    choices = likely or ports
    if len(choices) != 1:
        rendered = ", ".join(choices) if choices else "none found"
        raise RuntimeError(f"specify --port; detected ports: {rendered}")
    return choices[0]


def provision(port_name: str, frame: bytes, timeout: float) -> None:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError("pyserial is required: python3 -m pip install pyserial") from exc
    deadline = time.monotonic() + timeout
    with serial.Serial(port_name, 115200, timeout=0.25, write_timeout=2) as port:
        # Opening ESP32-C3 USB Serial/JTAG resets the chip on some hosts. Wait
        # until the application has installed its receiver before sending any
        # secret material; writes during ROM boot can otherwise block or vanish.
        ready = False
        ready_deadline = min(deadline, time.monotonic() + 8.0)
        while time.monotonic() < ready_deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line.endswith(f"{PROTOCOL} READY") or line == f"{PROTOCOL} READY":
                ready = True
                break
        # A device already sitting on the setup screen emitted READY before the
        # host opened the port. Eight quiet seconds also safely clears boot.
        try:
            port.write(frame)
        except serial.SerialTimeoutException as exc:
            raise RuntimeError("USB write timed out after the device became ready") from exc
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", errors="replace").strip()
            if line.endswith(f"{PROTOCOL} OK") or line == f"{PROTOCOL} OK":
                return
            if line.endswith(f"{PROTOCOL} ERROR") or line == f"{PROTOCOL} ERROR":
                raise RuntimeError("device rejected the application credentials")
    raise TimeoutError("device did not confirm provisioning; keep it on the private-app setup screen")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Write your own Feishu App ID/Secret to AI Passport over USB."
    )
    parser.add_argument("--app-id", required=True, help="owner's Feishu App ID")
    parser.add_argument("--port", help="USB serial port; auto-detected when unique")
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()
    secret = getpass.getpass("Feishu App Secret (input hidden): ")
    try:
        frame = build_frame(args.app_id.strip(), secret)
        secret = ""
        provision(choose_port(args.port), frame, args.timeout)
    except (RuntimeError, TimeoutError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    finally:
        secret = ""
    print("Feishu application saved. Complete the authorization QR on the device.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
