#!/usr/bin/env python3
import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path

from tools.analyze_iio_trace import parse_trace

ROOT = Path(__file__).resolve().parents[1]


def od_lines(scans):
    raw = b"".join(
        struct.pack("<iiiIq", 10, -20, 30, 0, timestamp)
        for timestamp in scans
    )
    fields = [f"{byte:02x}" for byte in raw]
    return "\n".join(
        " " + " ".join(fields[i : i + 16]) for i in range(0, len(fields), 16)
    )


class IioTraceAnalysisTest(unittest.TestCase):
    def report(self, timestamps, reported_hz):
        count = len(timestamps)
        data = od_lines(timestamps)
        return (
            "current_timestamp_clock: monotonic\n"
            "MOTION_CONTINUOUS sampling settings with buffer enabled:\n"
            f"/sys/bus/iio/devices/iio:device2/in_accel_sampling_frequency: {reported_hz}\n"
            "MOTION_CONTINUOUS start realtime=1234567800.0 uptime=90.0\n"
            f"MOTION_CONTINUOUS samples: {count} scans, {count * 24} bytes, dd_status=124\n"
            f"{data}\n"
            "MOTION_CONTINUOUS end realtime=1234567890.0 uptime=100.0\n"
            "/sys/bus/iio/devices/iio:device2/in_accel_sampling_frequency: 20\n"
        )

    def test_derives_rate_from_timestamps_not_sysfs_setting(self):
        scans, status, clock, intervals, reported, _, _ = parse_trace(
            self.report([1_000_000_000, 1_100_000_000, 1_200_000_000], 10)
        )
        self.assertEqual(scans, 3)
        self.assertEqual(status, 124)
        self.assertEqual(clock, "monotonic")
        self.assertEqual(intervals, [0.1, 0.1])
        self.assertEqual(reported, 10.0)

    def test_rejects_mismatched_byte_count(self):
        report = self.report([1_000_000_000, 1_100_000_000], 10)
        report = report.replace("2 scans, 48 bytes", "2 scans, 24 bytes")
        with self.assertRaisesRegex(ValueError, "trace hex contains"):
            parse_trace(report)

    def test_rejects_nonmonotonic_timestamps(self):
        with self.assertRaisesRegex(ValueError, "strictly increasing"):
            parse_trace(
                self.report([1_100_000_000, 1_000_000_000], 10)
            )

    def test_cli_flags_reported_rate_that_exceeds_timestamped_delivery(self):
        report = self.report([1_000_000_000, 1_300_000_000, 1_600_000_000], 10)
        with tempfile.NamedTemporaryFile(mode="w", encoding="ascii") as file:
            file.write(report)
            file.flush()
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/analyze_iio_trace.py"), file.name],
                capture_output=True,
                text=True,
                check=True,
            )
        self.assertIn("median interval 300.0 ms (3.333 Hz)", result.stdout)
        self.assertIn("WARNING: median timestamped delivery differs", result.stdout)

    def test_cli_reports_long_gap_and_realtime_offsets(self):
        first = 1_794_694_762_057_620_000
        second = 1_794_694_883_827_620_000
        report = self.report(
            [first, second, second + 100_000_000, second + 200_000_000], 10
        ).replace("current_timestamp_clock: monotonic", "current_timestamp_clock: realtime")
        report = report.replace(
            "realtime=1234567800.0", "realtime=1794694762.24125"
        )
        with tempfile.NamedTemporaryFile(mode="w", encoding="ascii") as file:
            file.write(report)
            file.flush()
            result = subprocess.run(
                [sys.executable, str(ROOT / "tools/analyze_iio_trace.py"), file.name],
                capture_output=True,
                text=True,
                check=True,
            )
        self.assertIn("median interval 100.0 ms (10.000 Hz)", result.stdout)
        self.assertIn("first inter-scan gap: 121.770 s", result.stdout)
        self.assertIn("largest gap: 121.770 s between scans 0 and 1", result.stdout)
        self.assertIn("first -0.184 s", result.stdout)
        self.assertNotIn("WARNING: median timestamped delivery differs", result.stdout)


if __name__ == "__main__":
    unittest.main()
