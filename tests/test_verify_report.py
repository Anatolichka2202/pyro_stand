"""Самостоятельные проверки архива без Qt и устройства."""
import base64
import hashlib
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MOD = importlib.util.spec_from_file_location("verify_report", ROOT / "tools" / "verify_report.py")
verifier = importlib.util.module_from_spec(MOD)
MOD.loader.exec_module(verifier)
MOD2 = importlib.util.spec_from_file_location("virtual_tester", ROOT / "tools" / "virtual_tester.py")
virtual = importlib.util.module_from_spec(MOD2)
MOD2.loader.exec_module(virtual)


class VerifyReportTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)
        self.trace = self.dir / "frames.jsonl"
        self.report_file = self.dir / "report.json"

    def make_report(self, frames, protocol="aa55-96channels"):
        lines = []
        total = 0
        rejected = 0
        for frame in frames:
            valid = (protocol != "aa55-96channels" or
                     (len(frame) == 963 and verifier.crc8(frame[:-1]) == frame[-1]))
            lines.append(json.dumps({
                "protocol": protocol,
                "valid": valid,
                "frame_base64": base64.b64encode(frame).decode("ascii")}))
            if valid:
                total += len(frame) if protocol == "legacy-mask-1ms" else 1
            else:
                rejected += 1
        evidence = ("\n".join(lines) + "\n").encode("utf-8")
        self.trace.write_bytes(evidence)
        cyclo = b"START_UTC_DATE=2026-10-16\n"
        report = {"format": "pyro_stand.report.v1",
                  "frame_evidence_file": self.trace.name,
                  "frame_evidence_sha256": hashlib.sha256(evidence).hexdigest(),
                  "tester_protocol": protocol,
                  "accepted_frames": str(total),
                  "rejected_frames": str(rejected),
                  "cyclogram_base64": base64.b64encode(cyclo).decode("ascii"),
                  "cyclogram_sha256": hashlib.sha256(cyclo).hexdigest(),
                  "collection_finished": True}
        self.report_file.write_text(json.dumps(report), encoding="utf-8")
        return report

    def test_modern_and_corrupted_crc(self):
        frames = virtual.modern_scenario("bad-crc")
        self.make_report(frames)
        result = verifier.validate(self.report_file)
        self.assertEqual(result["accepted"], 4)
        self.assertEqual(result["rejected"], 1)

    def test_legacy_bytes(self):
        self.make_report([virtual.old_scenario("normal")], "legacy-mask-1ms")
        result = verifier.validate(self.report_file)
        self.assertEqual(result["accepted"], 340)

    def test_modified_archive_is_rejected(self):
        self.make_report(virtual.modern_scenario("normal"))
        self.trace.write_bytes(self.trace.read_bytes() + b"\n")
        with self.assertRaises(verifier.ReportError):
            verifier.validate(self.report_file)

    def test_wrong_count_is_rejected(self):
        report = self.make_report(virtual.modern_scenario("normal"))
        report["accepted_frames"] = "999"
        self.report_file.write_text(json.dumps(report), encoding="utf-8")
        with self.assertRaises(verifier.ReportError):
            verifier.validate(self.report_file)


if __name__ == "__main__":
    unittest.main()
