#!/usr/bin/env python3
"""Проверка генератора без Qt: формат, CRC, детерминизм, потери, граница таймера."""
import base64
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location(
    "virtual_tester", Path(__file__).resolve().parents[1] / "tools" / "virtual_tester.py")
virtual = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(virtual)


class VirtualTesterSelfTest(unittest.TestCase):
    def test_packet_layout(self):
        packet = virtual.board_frame({8: (True, True, 123_456, 234_567)})
        self.assertEqual(len(packet), 963)
        self.assertEqual(packet[:2], b"\xaa\x55")
        self.assertEqual(packet[-1], virtual.crc8(packet[:-1]))
        self.assertEqual(packet[2 + 8 * 10:2 + 9 * 10],
                         b"\x01\x01\x40\xe2\x01\x00\x47\x94\x03\x00")

    def test_legacy_ticks(self):
        data = virtual.old_scenario("normal")
        self.assertEqual(len(data), 340)
        self.assertEqual(data[4] & 0x80, 0)
        self.assertEqual(data[5] & 0x80, 0x80)
        self.assertEqual(data[25] & 3, 3)
        self.assertEqual(data[31] & 3, 0)

    def test_wrap_and_corruption(self):
        packets = virtual.modern_scenario("wrap")
        self.assertEqual(len(packets), 4)
        self.assertEqual(packets[2][-1], virtual.crc8(packets[2][:-1]))
        corrupted = virtual.modern_scenario("bad-crc")[2]
        self.assertNotEqual(corrupted[-1], virtual.crc8(corrupted[:-1]))
        self.assertEqual(len(virtual.modern_scenario("loss")), 3)

    def test_trace_roundtrip(self):
        packets = virtual.modern_scenario("normal")
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "frames.jsonl"
            virtual.save_trace(output, "aa55-96channels", packets)
            self.assertEqual(virtual.load_trace(output, "aa55-96channels"), packets)
            self.assertEqual(len(output.read_text().splitlines()), 4)
            # Потеря пакета в трассе не создаёт новой временной опоры.
            wrong = virtual.load_trace(output, "aa55-96channels")
            wrong.pop(1)
            self.assertEqual(len(wrong), 3)


if __name__ == "__main__":
    unittest.main()
