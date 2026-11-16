#!/usr/bin/env python3
"""Report the timestamp-derived scan rate in a sensordiag continuous trace."""

import argparse
import re
import statistics
import struct
import sys
from pathlib import Path
from decimal import Decimal


TRACE_BYTES_PER_SCAN = 24
TIMESTAMP_OFFSET = 16
SAMPLE_SUMMARY = re.compile(
    r"MOTION_CONTINUOUS samples: (\d+) scans, (\d+) bytes, dd_status=(\d+)"
)
FREQUENCY_SETTING = re.compile(
    r"/in_accel_sampling_frequency:\s*([0-9]+(?:\.[0-9]+)?)\s*$"
)
TRACE_START = re.compile(r"MOTION_CONTINUOUS start realtime=([0-9]+(?:\.[0-9]+)?)")


def parse_trace(text):
    lines = text.splitlines()
    summary_index = next(
        (i for i, line in enumerate(lines) if SAMPLE_SUMMARY.search(line)), None
    )
    if summary_index is None:
        raise ValueError("continuous trace sample summary not found")
    summary = SAMPLE_SUMMARY.search(lines[summary_index])
    expected_scans, expected_bytes, dd_status = map(int, summary.groups())

    end_index = next(
        (
            i
            for i in range(summary_index + 1, len(lines))
            if lines[i].startswith("MOTION_CONTINUOUS end ")
        ),
        None,
    )
    if end_index is None:
        raise ValueError("continuous trace end marker not found")

    raw = bytearray()
    for line in lines[summary_index + 1 : end_index]:
        fields = line.split()
        if not fields:
            continue
        if any(not re.fullmatch(r"[0-9a-fA-F]{2}", field) for field in fields):
            raise ValueError(f"unexpected content in raw trace: {line}")
        raw.extend(int(field, 16) for field in fields)

    if len(raw) != expected_bytes:
        raise ValueError(
            f"trace hex contains {len(raw)} bytes; summary reports {expected_bytes}"
        )
    if expected_bytes != expected_scans * TRACE_BYTES_PER_SCAN:
        raise ValueError("trace summary byte count does not match 24-byte scan layout")
    if expected_scans == 0:
        raise ValueError("continuous trace has no scans")

    timestamps = [
        struct.unpack_from("<q", raw, i * TRACE_BYTES_PER_SCAN + TIMESTAMP_OFFSET)[0]
        for i in range(expected_scans)
    ]
    if any(timestamp <= 0 for timestamp in timestamps):
        raise ValueError("trace contains a non-positive IIO timestamp")
    intervals = [
        (right - left) / 1_000_000_000
        for left, right in zip(timestamps, timestamps[1:])
    ]
    if any(interval <= 0 for interval in intervals):
        raise ValueError("IIO timestamps are not strictly increasing")

    clock_match = re.search(r"current_timestamp_clock:\s*(\S+)", text)
    start_match = TRACE_START.search(text)
    active_frequency = None
    in_active_settings = False
    frequency_values = []
    for line in lines:
        if line.startswith("MOTION_CONTINUOUS sampling settings with buffer enabled:"):
            in_active_settings = True
            continue
        if in_active_settings and line.startswith("MOTION_CONTINUOUS "):
            in_active_settings = False
        match = FREQUENCY_SETTING.search(line)
        if match:
            frequency = float(match.group(1))
            frequency_values.append(frequency)
            if in_active_settings:
                active_frequency = frequency
    reported_frequency = (
        active_frequency if active_frequency is not None
        else frequency_values[-1] if frequency_values
        else None
    )
    return (
        expected_scans,
        dd_status,
        clock_match.group(1) if clock_match else "unknown",
        intervals,
        reported_frequency,
        timestamps,
        Decimal(start_match.group(1)) if start_match else None,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "sensors_txt", type=Path, help="sensors.txt path, or - to read from stdin"
    )
    args = parser.parse_args()
    try:
        text = (
            sys.stdin.read()
            if str(args.sensors_txt) == "-"
            else args.sensors_txt.read_text(errors="replace")
        )
        scans, dd_status, clock, intervals, reported, timestamps, start = parse_trace(
            text
        )
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    print(f"scans: {scans}; dd_status: {dd_status}; IIO timestamp clock: {clock}")
    if intervals:
        elapsed = sum(intervals)
        average_rate = len(intervals) / elapsed
        median_interval = statistics.median(intervals)
        median_rate = 1 / median_interval
        largest_gap_index = max(range(len(intervals)), key=intervals.__getitem__)
        print(
            "timestamp-derived average over full trace: "
            f"{average_rate:.3f} Hz over {elapsed:.3f} s; "
            f"median interval {median_interval * 1000:.1f} ms "
            f"({median_rate:.3f} Hz); "
            f"min/max {min(intervals) * 1000:.1f}/{max(intervals) * 1000:.1f} ms"
        )
        print(
            f"first inter-scan gap: {intervals[0]:.3f} s; "
            f"largest gap: {intervals[largest_gap_index]:.3f} s "
            f"between scans {largest_gap_index} and {largest_gap_index + 1}"
        )
        if clock == "realtime" and start is not None:
            ns = Decimal(1_000_000_000)
            first_offset = Decimal(timestamps[0]) / ns - start
            last_offset = Decimal(timestamps[-1]) / ns - start
            print(
                "IIO timestamp offsets from trace-start log marker: "
                f"first {first_offset:+.3f} s; last {last_offset:+.3f} s"
            )
        if reported is not None:
            print(f"reported IIO sampling-frequency setting: {reported:g} Hz")
            if reported <= 0:
                print("WARNING: reported IIO sampling-frequency setting is not positive")
            elif abs(median_rate - reported) / reported > 0.25:
                print(
                    "WARNING: median timestamped delivery differs from the sysfs "
                    "setting; the setting alone does not establish delivered scan rate"
                )
    else:
        print("timestamp-derived rate: unavailable (fewer than two scans)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
