#!/usr/bin/env python3
"""
Monitor XInput rumble output reports sent by Windows to a USB gamepad.

XInput does not expose a "get current vibration" API. This script watches USB
traffic with tshark/USBPcap and prints XInput OUT reports that look like the
Xbox 360 rumble command handled by receiver/main/usb_xinput.c:

    00 08 00 LL RR 00 00 00

where LL/RR are the left and right motor strengths, 0-255.

Requirements:
    - Windows
    - Wireshark/tshark installed with USBPcap
    - Run from an elevated terminal if USBPcap capture requires admin rights

Usage:
    py tools/windows_monitor_rumble.py --list-interfaces
    py tools/windows_monitor_rumble.py --interface USBPcap1
    py tools/windows_monitor_rumble.py --interface USBPcap1 --changes-only
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable


DEFAULT_ENDPOINT = 0x01
RUMBLE_REPORT_LEN = 8


@dataclasses.dataclass(frozen=True)
class RumbleEvent:
    left: int
    right: int
    raw_hex: str


def _normalize_hex(values: Iterable[int]) -> str:
    return " ".join(f"{value:02X}" for value in values)


def _parse_hex_bytes(text: str) -> list[int]:
    text = text.strip()
    if not text:
        return []

    normalized = re.sub(r"[\s,:;-]+", " ", text)
    tokens = [token for token in normalized.split(" ") if token]

    if len(tokens) == 1:
        compact = tokens[0]
        if compact.lower().startswith("0x"):
            compact = compact[2:]
        if (
            len(compact) > 2
            and len(compact) % 2 == 0
            and re.fullmatch(r"[0-9a-fA-F]+", compact)
        ):
            return [int(compact[i : i + 2], 16) for i in range(0, len(compact), 2)]

    values: list[int] = []
    for token in tokens:
        if token.lower().startswith("0x"):
            token = token[2:]
        if len(token) != 2 or not re.fullmatch(r"[0-9a-fA-F]{2}", token):
            return []
        values.append(int(token, 16))
    return values


def parse_usb_payload(payload_hex: str) -> RumbleEvent | None:
    payload = _parse_hex_bytes(payload_hex)
    if len(payload) < RUMBLE_REPORT_LEN:
        return None

    if payload[0] != 0x00 or payload[1] != 0x08:
        return None

    return RumbleEvent(
        left=payload[3], right=payload[4], raw_hex=_normalize_hex(payload)
    )


def format_rumble_event(event: RumbleEvent) -> str:
    left_pct = event.left * 100.0 / 255.0
    right_pct = event.right * 100.0 / 255.0
    return (
        f"rumble left={event.left} ({left_pct:.1f}%) "
        f"right={event.right} ({right_pct:.1f}%) raw={event.raw_hex}"
    )


def parse_endpoint_arg(value: str) -> int | None:
    if value.lower() == "any":
        return None
    return int(value, 0)


def _extract_endpoint(endpoint_text: str) -> int | None:
    match = re.search(r"0x[0-9a-fA-F]+|\d+", endpoint_text)
    if not match:
        return None
    return int(match.group(0), 0)


def _endpoint_matches(endpoint_text: str, expected: int | None) -> bool:
    if expected is None:
        return True
    actual = _extract_endpoint(endpoint_text)
    if actual is None:
        return True
    return actual == expected


def find_tshark(tshark: str) -> str | None:
    if any(sep in tshark for sep in ("\\", "/")):
        return tshark if shutil.which(tshark) or os.path.exists(tshark) else None
    return shutil.which(tshark)


def decode_process_output(output: bytes | str | None) -> str:
    if output is None:
        return ""
    if isinstance(output, str):
        return output
    return output.decode("utf-8", errors="replace")


def list_tshark_interfaces(tshark: str) -> tuple[int, str]:
    result = subprocess.run(
        [tshark, "-D"],
        check=False,
        capture_output=True,
    )
    output = decode_process_output(result.stdout)
    output += decode_process_output(result.stderr)
    return result.returncode, output


def usbpcap_interfaces(interface_output: str) -> list[str]:
    interfaces: list[str] = []
    for line in interface_output.splitlines():
        match = re.search(r"\bUSBPcap\d+\b", line)
        if match and match.group(0) not in interfaces:
            interfaces.append(match.group(0))
    return interfaces


def choose_interface(tshark: str, requested: str | None) -> str | None:
    if requested:
        return requested

    rc, output = list_tshark_interfaces(tshark)
    if rc != 0:
        print(output, file=sys.stderr)
        return None

    interfaces = usbpcap_interfaces(output)
    if len(interfaces) == 1:
        return interfaces[0]

    if not interfaces:
        print(
            "ERROR: No USBPcap interface was found. Install Wireshark with USBPcap, "
            "then run with --list-interfaces.",
            file=sys.stderr,
        )
        return None

    print(
        "ERROR: Multiple USBPcap interfaces found. Choose one with --interface:",
        file=sys.stderr,
    )
    for name in interfaces:
        print(f"  {name}", file=sys.stderr)
    return None


def build_tshark_command(tshark: str, interface: str) -> list[str]:
    return [
        tshark,
        "-l",
        "-i",
        interface,
        "-Y",
        "usb.capdata",
        "-T",
        "fields",
        "-E",
        "separator=\t",
        "-E",
        "occurrence=f",
        "-e",
        "frame.time_relative",
        "-e",
        "usb.endpoint_address",
        "-e",
        "usb.capdata",
    ]


def iter_rumble_events(
    lines: Iterable[str], endpoint: int | None
) -> Iterable[RumbleEvent]:
    for line in lines:
        fields = line.rstrip("\r\n").split("\t")
        if len(fields) < 3:
            continue

        _, endpoint_text, payload_hex = fields[:3]
        if not payload_hex or not _endpoint_matches(endpoint_text, endpoint):
            continue

        event = parse_usb_payload(payload_hex)
        if event:
            yield event


def monitor(args: argparse.Namespace) -> int:
    tshark = find_tshark(args.tshark)
    if tshark is None:
        print(
            "ERROR: tshark was not found. Install Wireshark with USBPcap, "
            "or pass --tshark C:\\path\\to\\tshark.exe.",
            file=sys.stderr,
        )
        return 2

    if args.list_interfaces:
        rc, output = list_tshark_interfaces(tshark)
        print(output, end="" if output.endswith("\n") else "\n")
        return rc

    interface = choose_interface(tshark, args.interface)
    if interface is None:
        return 2

    endpoint = parse_endpoint_arg(args.endpoint)
    endpoint_label = "any" if endpoint is None else f"0x{endpoint:02X}"

    print(
        f"Monitoring {interface} for XInput rumble OUT reports on endpoint {endpoint_label}. "
        "Press Ctrl+C to stop.",
        flush=True,
    )

    command = build_tshark_command(tshark, interface)
    last_event: tuple[int, int] | None = None

    try:
        with subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        ) as proc:
            assert proc.stdout is not None
            for event in iter_rumble_events(proc.stdout, endpoint):
                current = (event.left, event.right)
                if args.changes_only and current == last_event:
                    continue

                print(format_rumble_event(event), flush=True)
                last_event = current

            return proc.wait()
    except KeyboardInterrupt:
        return 130
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Monitor Windows XInput rumble packets with tshark/USBPcap."
    )
    parser.add_argument(
        "-i",
        "--interface",
        help="USBPcap capture interface, for example USBPcap1.",
    )
    parser.add_argument(
        "--list-interfaces",
        action="store_true",
        help="Print tshark capture interfaces and exit.",
    )
    parser.add_argument(
        "--endpoint",
        default=f"0x{DEFAULT_ENDPOINT:02X}",
        help="USB endpoint address to accept, or 'any' (default: 0x01).",
    )
    parser.add_argument(
        "--changes-only",
        action="store_true",
        help="Only print when left/right motor values change.",
    )
    parser.add_argument(
        "--tshark",
        default="tshark",
        help="Path to tshark.exe, or command name on PATH (default: tshark).",
    )
    args = parser.parse_args()
    return monitor(args)


if __name__ == "__main__":
    raise SystemExit(main())
