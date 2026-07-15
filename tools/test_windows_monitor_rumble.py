import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))

import windows_monitor_rumble as monitor


class RumbleParsingTests(unittest.TestCase):
    def test_parses_xinput_rumble_output_report(self):
        event = monitor.parse_usb_payload("00 08 00 40 80 00 00 00")

        self.assertIsNotNone(event)
        self.assertEqual(event.left, 0x40)
        self.assertEqual(event.right, 0x80)
        self.assertEqual(event.raw_hex, "00 08 00 40 80 00 00 00")

    def test_parses_tshark_colon_separated_payload(self):
        event = monitor.parse_usb_payload("00:08:00:01:ff:00:00:00")

        self.assertIsNotNone(event)
        self.assertEqual(event.left, 1)
        self.assertEqual(event.right, 255)

    def test_ignores_non_xinput_output_report(self):
        self.assertIsNone(monitor.parse_usb_payload("01 14 00 00 00 00 00 00"))

    def test_ignores_short_payload(self):
        self.assertIsNone(monitor.parse_usb_payload("00 08 00 40"))

    def test_formats_event_with_percentages(self):
        event = monitor.RumbleEvent(left=128, right=255, raw_hex="00 08 00 80 FF 00 00 00")

        self.assertEqual(
            monitor.format_rumble_event(event),
            "rumble left=128 (50.2%) right=255 (100.0%) raw=00 08 00 80 FF 00 00 00",
        )

    def test_decodes_tshark_interface_output_from_bytes(self):
        original_run = monitor.subprocess.run

        def fake_run(*args, **kwargs):
            self.assertFalse(kwargs.get("text", False))
            return subprocess.CompletedProcess(
                args=args[0],
                returncode=0,
                stdout=b"1. USBPcap1 (USBPcap1)\n2. example \xac adapter\n",
                stderr=b"",
            )

        monitor.subprocess.run = fake_run
        try:
            rc, output = monitor.list_tshark_interfaces("tshark")
        finally:
            monitor.subprocess.run = original_run

        self.assertEqual(rc, 0)
        self.assertIsInstance(output, str)
        self.assertIn("USBPcap1", output)

    def test_serial_forwarding_api_is_removed(self):
        self.assertFalse(hasattr(monitor, "WindowsSerialPort"))
        self.assertFalse(hasattr(monitor, "send_serial_if_triggered"))
        self.assertFalse(hasattr(monitor, "parse_serial_trigger"))


if __name__ == "__main__":
    unittest.main()
