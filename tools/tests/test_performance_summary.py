#!/usr/bin/env python3
"""Verify schema parsing and arithmetic with explicitly synthetic samples."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("performance", REPO / "tools/summarize_performance.py")
performance = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(performance)
FIXTURES = REPO / "tools/tests/fixtures"


class PerformanceSummaryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.fps = Path(self.temporary.name) / "fps.csv"
        self.profile = Path(self.temporary.name) / "profile.csv"
        self.fps.write_bytes((FIXTURES / "performance-fpschart-synthetic.csv").read_bytes())
        self.profile.write_bytes((FIXTURES / "performance-profile-synthetic.csv").read_bytes())
        self.context = json.loads((FIXTURES / "performance-context-synthetic.json").read_text())

    def summary(self):
        return performance.summarize(self.fps, self.profile, self.context)

    def test_raw_frames_exclude_percentile_prelude_and_use_final_profile_header(self):
        result = self.summary()
        self.assertEqual(result["measurement_origin"], "synthetic fixture")
        self.assertEqual(result["fps_chart"]["frames"], 5)
        self.assertAlmostEqual(result["fps_chart"]["aggregate_fps"], 50)
        self.assertEqual(result["fps_chart"]["frame_ms"]["mean"], 20)
        self.assertEqual(result["fps_chart"]["frame_ms"]["p95"], 30)
        self.assertEqual(result["fps_chart"]["gpu_ms_available_samples"]["mean"], 9)
        self.assertEqual(result["fps_chart"]["gpu_ms_available_samples"]["p95"], 14)
        self.assertEqual(result["csv_profile"]["process_physical_mib"]["mean"], 600)
        self.assertEqual(result["csv_profile"]["process_physical_mib"]["maximum"], 700)
        self.assertTrue(result["counter_coverage_complete"])

    def test_truncated_capture_without_finalized_metadata_is_rejected(self):
        self.profile.write_text(self.profile.read_text().split("[HasHeaderRowAtEnd]")[0])
        with self.assertRaisesRegex(ValueError, "finalized"):
            self.summary()

    def test_missing_memory_counter_is_rejected(self):
        self.profile.write_text(self.profile.read_text().replace("PhysicalUsedMB", "UnrelatedCounter"))
        with self.assertRaisesRegex(ValueError, "PhysicalUsedMB"):
            self.summary()

    def test_nonfinite_and_zero_frame_durations_are_rejected(self):
        for invalid in ("nan", "inf", "0", "-1"):
            original = (FIXTURES / "performance-fpschart-synthetic.csv").read_text()
            self.fps.write_text(original.replace("0.00,10,", f"0.00,{invalid},"))
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                self.summary()

    def test_missing_gpu_is_partial_coverage_instead_of_zero_cost(self):
        self.fps.write_text(self.fps.read_text().replace("0.00,10,3,4,5,", "0.00,10,3,4,0,"))
        result = self.summary()
        self.assertFalse(result["counter_coverage_complete"])
        self.assertEqual(result["fps_chart"]["gpu_unavailable_samples"], 1)
        self.assertEqual(result["fps_chart"]["gpu_ms_available_samples"]["samples"], 4)
        self.assertEqual(result["fps_chart"]["gpu_ms_available_samples"]["mean"], 10)

    def test_cli_writes_partial_summary_and_returns_nonzero_for_missing_gpu(self):
        self.fps.write_text(self.fps.read_text().replace("0.00,10,3,4,5,", "0.00,10,3,4,0,"))
        context = Path(self.temporary.name) / "context.json"
        context.write_text(json.dumps(self.context))
        output = Path(self.temporary.name) / "summary.json"
        process = subprocess.run([sys.executable, str(REPO / "tools/summarize_performance.py"),
                                  "--fps-chart", str(self.fps), "--csv-profile", str(self.profile),
                                  "--context", str(context), "--output", str(output)], capture_output=True, text=True)
        self.assertEqual(process.returncode, 2)
        self.assertFalse(json.loads(output.read_text())["counter_coverage_complete"])
        self.assertIn("incomplete counter evidence", process.stderr)

    def test_zero_memory_and_missing_context_cannot_be_evidence(self):
        self.profile.write_text(self.profile.read_text().replace(",10,500", ",10,0"))
        with self.assertRaisesRegex(ValueError, "PhysicalUsedMB"):
            self.summary()
        del self.context["configuration"]
        with self.assertRaisesRegex(ValueError, "configuration"):
            self.summary()

    def test_shipping_is_not_mislabeled_as_supported_counter_capture(self):
        self.context["configuration"] = "Shipping"
        with self.assertRaisesRegex(ValueError, "Development or Test"):
            self.summary()


if __name__ == "__main__":
    unittest.main()
