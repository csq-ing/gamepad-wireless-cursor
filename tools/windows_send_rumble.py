#!/usr/bin/env python3
"""
Send XInput rumble to an Xbox 360 controller on Windows.

Uses the XInput API (xinput1_4.dll) so it works with XUSB-matched
vendor-class devices. The old HID API approach does not apply here.

Usage:
    py windows_send_rumble.py                   # default: both motors at 50 %, controller 0
    py windows_send_rumble.py --left 100 --right 0 --index 0
    py windows_send_rumble.py --left 0 --right 0   # stop
"""

from __future__ import annotations

import argparse
import ctypes
import struct
import sys
import time


class XINPUT_VIBRATION(ctypes.Structure):
    _fields_ = [
        ("wLeftMotorSpeed", ctypes.c_ushort),
        ("wRightMotorSpeed", ctypes.c_ushort),
    ]


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [
        ("dwPacketNumber", ctypes.c_ulong),
        ("Gamepad_wButtons", ctypes.c_ushort),
        ("Gamepad_bLeftTrigger", ctypes.c_ubyte),
        ("Gamepad_bRightTrigger", ctypes.c_ubyte),
        ("Gamepad_sThumbLX", ctypes.c_short),
        ("Gamepad_sThumbLY", ctypes.c_short),
        ("Gamepad_sThumbRX", ctypes.c_short),
        ("Gamepad_sThumbRY", ctypes.c_short),
    ]


def load_xinput():
    for dll_name in ("xinput1_4", "xinput1_3", "xinput9_1_0"):
        try:
            return ctypes.windll.LoadLibrary(dll_name)
        except OSError:
            continue
    print("ERROR: Cannot load any XInput DLL.", file=sys.stderr)
    sys.exit(2)


def pct_to_u16(pct: int) -> int:
    return int(pct * 65535 / 100)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Send XInput rumble to an Xbox 360 controller."
    )
    parser.add_argument(
        "--index",
        type=int,
        default=0,
        choices=range(4),
        help="Controller index 0-3 (default 0)",
    )
    parser.add_argument(
        "--left", type=int, default=100, help="Left motor strength 0-100%% (default 50)"
    )
    parser.add_argument(
        "--right",
        type=int,
        default=100,
        help="Right motor strength 0-100%% (default 50)",
    )
    parser.add_argument(
        "--duration", type=float, default=2.0, help="Duration in seconds (default 2.0)"
    )
    args = parser.parse_args()

    xinput = load_xinput()

    state = XINPUT_STATE()
    rc = xinput.XInputGetState(args.index, ctypes.byref(state))
    if rc != 0:
        print(f"XInputGetState({args.index}) failed with error {rc}.")
        print("Make sure the controller is connected and recognised as Xbox 360.")
        return 1

    print(f"Controller {args.index} detected (packet={state.dwPacketNumber})")

    vib = XINPUT_VIBRATION(pct_to_u16(args.left), pct_to_u16(args.right))
    print(
        f"Setting rumble: left={args.left}% ({vib.wLeftMotorSpeed}) "
        f"right={args.right}% ({vib.wRightMotorSpeed})"
    )

    rc = xinput.XInputSetState(args.index, ctypes.byref(vib))
    if rc != 0:
        print(f"XInputSetState failed with error {rc}", file=sys.stderr)
        return 1

    print(f"Rumble active for {args.duration:.1f}s ...")
    time.sleep(args.duration)

    vib_off = XINPUT_VIBRATION(0, 0)
    xinput.XInputSetState(args.index, ctypes.byref(vib_off))
    print("Rumble stopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
