#!/usr/bin/env python3
"""Summarize UE 5.8 raw FPSChart + synchronized CsvProfile captures.

Schema sources: Engine/Private/ChartCreation.cpp::DumpFrameTimesToStatsLog
and Core/Private/ProfilingDebugging/CsvProfiler.cpp::FCsvStreamWriter::Finalize.
This tool records measurements and coverage; it never closes an audit item.
"""
import argparse
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import statistics


FPS_HEADER = ["Time (sec)", "Frame (ms)", "GT (ms)", "RT (ms)", "GPU (ms)", "DynRes", "Context"]


def rows_from(path):
    opener = gzip.open if path.name.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8-sig", newline="") as source:
        return [[cell.strip() for cell in row] for row in csv.reader(source) if row]


def number(value, label, positive=False):
    result = float(value)
    if not math.isfinite(result) or result < 0 or (positive and result == 0):
        raise ValueError(f"{label} must be finite and {'positive' if positive else 'nonnegative'}")
    return result


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    return {"samples": len(values), "mean": statistics.fmean(values),
            "p95": ordered[math.ceil(0.95 * len(ordered)) - 1], "maximum": ordered[-1]}


def read_fps_chart(path):
    rows = rows_from(path)
    # The file begins with three precomputed percentile rows. Only the raw
    # section after Time(sec) is a frame population; never count the prelude.
    headers = [i for i, row in enumerate(rows) if row == FPS_HEADER]
    if len(headers) != 1:
        raise ValueError("Expected one UE FPSChart raw 'Time (sec)' header")
    frames = []
    previous_time = -1.0
    for row in rows[headers[0] + 1:]:
        if len(row) != len(FPS_HEADER):
            raise ValueError("Truncated/malformed FPSChart frame row")
        timestamp = number(row[0], "FPSChart timestamp")
        if timestamp < previous_time:
            raise ValueError("FPSChart timestamps run backwards")
        previous_time = timestamp
        frames.append({"frame_ms": number(row[1], "Frame (ms)", positive=True),
                       "game_ms": number(row[2], "GT (ms)"),
                       "render_ms": number(row[3], "RT (ms)"),
                       "gpu_ms": number(row[4], "GPU (ms)")})
    if not frames:
        raise ValueError("FPSChart contains no raw frames")
    return frames


def read_csv_profile(path):
    rows = rows_from(path)
    headers = [i for i, row in enumerate(rows) if row and row[0] == "EVENTS"]
    if len(headers) != 2 or headers[0] != 0:
        raise ValueError("Expected initial and final CsvProfile EVENTS headers; capture may be incomplete")
    final_index = headers[-1]
    header = rows[final_index]
    if len(set(header)) != len(header):
        raise ValueError("Duplicate CsvProfile column names")
    if not {"FrameTime", "PhysicalUsedMB"}.issubset(header):
        raise ValueError("CsvProfile lacks FrameTime or PhysicalUsedMB")
    metadata_cells = [cell for row in rows[final_index + 1:] for cell in row]
    if len(metadata_cells) % 2:
        raise ValueError("Malformed CsvProfile metadata footer")
    metadata = dict(zip(metadata_cells[::2], metadata_cells[1::2]))
    if metadata.get("[HasHeaderRowAtEnd]") != "1":
        raise ValueError("CsvProfile has no finalized capture footer")
    frame_index, memory_index = header.index("FrameTime"), header.index("PhysicalUsedMB")
    frames = []
    for row in rows[1:final_index]:
        # New series may appear during capture. The final header is
        # authoritative; earlier rows can legitimately have fewer columns.
        if len(row) > len(header) or len(row) <= max(frame_index, memory_index):
            raise ValueError("CsvProfile row does not contain required counters")
        frames.append({"frame_ms": number(row[frame_index], "CsvProfile FrameTime", positive=True),
                       "physical_mib": number(row[memory_index], "PhysicalUsedMB", positive=True)})
    if not frames:
        raise ValueError("CsvProfile contains no frame counters")
    return frames, metadata


def summarize(fps_path, profile_path, context):
    for key in ("configuration", "hardware", "rhi", "resolution", "quality", "route", "capture_seconds",
                "engine_revision", "project_revision"):
        if key not in context or context[key] is None or context[key] == "":
            raise ValueError(f"Capture context must record {key}")
    if context["configuration"] not in ("Development", "Test"):
        raise ValueError("This stock-target counter recipe requires a Development or Test package")
    if not isinstance(context["resolution"], list) or len(context["resolution"]) != 2 or not all(
            type(value) is int and value > 0 for value in context["resolution"]):
        raise ValueError("Capture resolution must be [positive integer width, positive integer height]")
    number(context["capture_seconds"], "Observed capture duration", positive=True)
    fps = read_fps_chart(fps_path)
    profile, metadata = read_csv_profile(profile_path)
    frame_ms = [frame["frame_ms"] for frame in fps]
    gpu_ms = [frame["gpu_ms"] for frame in fps if frame["gpu_ms"] > 0]
    missing_gpu = len(fps) - len(gpu_ms)
    elapsed = sum(frame_ms) / 1000.0
    return {
        "context": context,
        "measurement_origin": "synthetic fixture" if context.get("synthetic") else "supplied capture",
        "sources": [{"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in (fps_path, profile_path)],
        "fps_chart": {"frames": len(fps), "sampled_seconds_from_rounded_frame_ms": elapsed,
                      "aggregate_fps": len(fps) / elapsed,
                      "frame_ms": distribution(frame_ms),
                      "game_ms": distribution([frame["game_ms"] for frame in fps]),
                      "render_ms": distribution([frame["render_ms"] for frame in fps]),
                      "gpu_ms_available_samples": distribution(gpu_ms),
                      "gpu_unavailable_samples": missing_gpu},
        "csv_profile": {"frames": len(profile),
                        "sampled_seconds": sum(frame["frame_ms"] for frame in profile) / 1000.0,
                        "process_physical_mib": distribution([frame["physical_mib"] for frame in profile]),
                        "platform_metadata": metadata.get("[platform]")},
        "counter_coverage_complete": missing_gpu == 0,
        "method": {"fps": "N / sum(raw frame durations in seconds), not mean instantaneous FPS",
                   "p95": "Nearest rank: sorted samples[ceil(0.95*N)-1]",
                   "means": "Arithmetic sample means; memory is frame-sampled, not time-weighted",
                   "memory": "PhysicalUsedMB = platform UsedPhysical / 1024^2; on Mac this is task phys_footprint, not RSS or dedicated VRAM",
                   "gpu": "Resolved RHI GPU frame durations may lag CPU frames; zero samples are unavailable, never counted as zero-cost GPU work",
                   "alignment": "FPSChart and CsvProfile started/stopped by DoCsvProfile; no per-frame join is assumed",
                   "scope": "Configuration-specific measurements only. No Shipping equivalence, performance budget verdict, or audit closure."},
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fps-chart", type=Path, required=True)
    parser.add_argument("--csv-profile", type=Path, required=True)
    parser.add_argument("--context", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = summarize(args.fps_chart, args.csv_profile, json.loads(args.context.read_text(encoding="utf-8")))
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, f"Performance summary rejected: {error}\n")
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    if not result["counter_coverage_complete"]:
        parser.exit(2, "Partial summary written: GPU samples are unavailable; this is incomplete counter evidence.\n")
