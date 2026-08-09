"""Reproducible audit of RaceBox trace, stationary zones, IMU zero, and R6 drift.

This script is deliberately read-only. It discovers the user's unique VBO/RaceBox
exports, keeps every session's place-down/race-start/race-stop landmarks independent,
and mirrors the native directed-crossing and whole-lap-translation calculations.
"""

from __future__ import annotations

import bisect
import csv
import glob
import hashlib
import json
import math
import os
import re
import statistics
from pathlib import Path


_configured_roots = os.environ.get("RACEBOX_AUDIT_SOURCE_ROOTS", "").strip()
SOURCE_ROOTS = tuple(
    Path(item).expanduser()
    for item in _configured_roots.split(os.pathsep)
    if item.strip()
) or (
    Path.home() / "Downloads",
    Path.home() / "Documents" / "datalogger",
    Path.home() / "Documents" / "race box",
)


def known_session_type(vbo: Path) -> str:
    """User-supplied July 11 classification; do not infer missing-day history."""
    if "11-07-2026 14-49" in vbo.name:
        return "race"
    if "11-07-2026 11-47" in vbo.name or "11-07-2026 13-19" in vbo.name:
        return "qualifying_or_practice"
    return "unknown"


def planar_distance(a: dict, b: dict) -> float:
    latitude = (a["lat"] + b["lat"]) * 0.5
    north = (b["lat"] - a["lat"]) * 110_540.0
    east = (b["lon"] - a["lon"]) * 111_320.0 * math.cos(math.radians(latitude))
    return math.hypot(east, north)


def percentile95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(0.95 * len(ordered)))]


def unique_vbo_files() -> list[Path]:
    result: list[Path] = []
    hashes: set[str] = set()
    for root in SOURCE_ROOTS:
        for name in glob.glob(os.path.join(root, "**", "*.vbo"), recursive=True):
            path = Path(name)
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            if digest not in hashes:
                hashes.add(digest)
                result.append(path)
    return sorted(result)


def matching_csv(vbo: Path) -> Path | None:
    base = re.sub(r" \(\d+\)$", "", vbo.stem)
    local = vbo.with_name(base + ".csv")
    if local.exists():
        return local
    for root in SOURCE_ROOTS:
        matches = list(root.glob(f"**/{base}.csv"))
        if matches:
            return matches[0]
    return None


def read_vbo(path: Path) -> tuple[list[dict], tuple[tuple[float, float], tuple[float, float]]]:
    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    line_definition = next(line for line in lines if line.startswith("Start "))
    values = re.findall(r"[+-]\d+(?:\.\d+)?", line_definition)
    start_line = (
        (float(values[0]) / 60.0, -abs(float(values[1]) / 60.0)),
        (float(values[2]) / 60.0, -abs(float(values[3]) / 60.0)),
    )
    samples: list[dict] = []
    for line in lines[lines.index("[data]") + 1 :]:
        fields = line.split()
        if len(fields) < 13:
            continue
        samples.append(
            {
                "lat": float(fields[1]) / 60.0,
                "lon": -abs(float(fields[2]) / 60.0),
                "speed": float(fields[3]),
                "heading": float(fields[4]),
                "altitude": float(fields[5]),
                "long_g": float(fields[6]),
                "lat_g": float(fields[7]),
                "vertical_g": float(fields[8]),
                "gyro_x": float(fields[9]),
                "gyro_y": float(fields[10]),
                "gyro_z": float(fields[11]),
                "satellites": int(float(fields[12])),
            }
        )
    return samples, start_line


def racebox_header_offset(path: Path) -> tuple[int, int]:
    lines = path.read_text(encoding="utf-8-sig", errors="ignore").splitlines()
    header_index = next(index for index, line in enumerate(lines) if line.startswith("Record,"))
    return header_index, len(list(csv.DictReader(lines[header_index:])))


def stationary_blocks(samples: list[dict]) -> list[tuple[int, int]]:
    """Mirror the native <=1.5 km/h and >=2.0 s (50 sample) rule."""
    result: list[tuple[int, int]] = []
    begin: int | None = None
    for index, sample in enumerate(samples):
        if sample["speed"] <= 1.5:
            if begin is None:
                begin = index
        elif begin is not None:
            if index - begin >= 50:
                result.append((begin, index - 1))
            begin = None
    if begin is not None and len(samples) - begin >= 50:
        result.append((begin, len(samples) - 1))
    return result


def zone_summary(samples: list[dict], begin: int, end: int) -> dict:
    selected = samples[begin : end + 1]
    latitude = statistics.median(sample["lat"] for sample in selected)
    longitude = statistics.median(sample["lon"] for sample in selected)
    center = {"lat": latitude, "lon": longitude}
    spatial_errors = [planar_distance(center, sample) for sample in selected]
    altitude = statistics.median(sample["altitude"] for sample in selected)
    altitude_errors = [abs(sample["altitude"] - altitude) for sample in selected]
    return {
        "begin": begin,
        "end": end,
        "seconds": len(selected) / 25.0,
        "lat": latitude,
        "lon": longitude,
        "horizontal_p95_m": percentile95(spatial_errors),
        "altitude_m": altitude,
        "altitude_p95_deviation_m": percentile95(altitude_errors),
        "median_satellites": statistics.median(sample["satellites"] for sample in selected),
    }


def side_of_line(sample: dict, a: tuple[float, float], b: tuple[float, float]) -> float:
    return (sample["lon"] - a[1]) * (b[0] - a[0]) - (sample["lat"] - a[0]) * (b[1] - a[1])


def orientation(ax: float, ay: float, bx: float, by: float, cx: float, cy: float) -> float:
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)


def segments_intersect(p: dict, q: dict, a: tuple[float, float], b: tuple[float, float]) -> bool:
    o1 = orientation(p["lon"], p["lat"], q["lon"], q["lat"], a[1], a[0])
    o2 = orientation(p["lon"], p["lat"], q["lon"], q["lat"], b[1], b[0])
    o3 = orientation(a[1], a[0], b[1], b[0], p["lon"], p["lat"])
    o4 = orientation(a[1], a[0], b[1], b[0], q["lon"], q["lat"])
    return ((o1 <= 0.0 <= o2) or (o2 <= 0.0 <= o1)) and ((o3 <= 0.0 <= o4) or (o4 <= 0.0 <= o3))


def directed_crossings(samples: list[dict], start_line: tuple) -> tuple[list[tuple], list[tuple]]:
    a, b = start_line
    events: list[tuple[int, int, float]] = []
    for index in range(1, len(samples)):
        before = side_of_line(samples[index - 1], a, b)
        after = side_of_line(samples[index], a, b)
        changed_side = (before < 0.0 <= after) or (before > 0.0 >= after)
        if changed_side and segments_intersect(samples[index - 1], samples[index], a, b):
            events.append((index, 1 if after > before else -1, samples[index]["speed"]))
    direction_sum = sum(direction for _, direction, speed in events if speed >= 5.0)
    forward = 1 if direction_sum >= 0 else -1
    accepted: list[tuple[int, int, float]] = []
    for event in events:
        if event[2] < 5.0 or event[1] != forward:
            continue
        if accepted and (event[0] - accepted[-1][0]) * 0.04 < 8.0:
            continue
        accepted.append(event)
    return events, accepted


def haversine(a: dict, b: dict) -> float:
    lat1, lat2 = math.radians(a["lat"]), math.radians(b["lat"])
    delta_latitude = lat2 - lat1
    delta_longitude = math.radians(b["lon"] - a["lon"])
    h = math.sin(delta_latitude / 2.0) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(delta_longitude / 2.0) ** 2
    return 6_371_000.0 * 2.0 * math.atan2(math.sqrt(h), math.sqrt(max(0.0, 1.0 - h)))


def lap_view(samples: list[dict], bounds: tuple[int, int]) -> tuple[int, int, list[float]]:
    begin, end = bounds
    cumulative = [0.0]
    for index in range(begin + 1, end + 1):
        cumulative.append(cumulative[-1] + haversine(samples[index - 1], samples[index]))
    return begin, end, cumulative


def point_at_progress(samples: list[dict], view: tuple, progress: float, frame: tuple[float, float]) -> tuple[float, float]:
    begin, end, cumulative = view
    target = progress * cumulative[-1]
    upper = bisect.bisect_left(cumulative, target)
    if upper == 0:
        index, ratio = begin, 0.0
    elif upper >= len(cumulative):
        index, ratio = end, 0.0
    else:
        lower = upper - 1
        span = cumulative[upper] - cumulative[lower]
        index = begin + lower
        ratio = (target - cumulative[lower]) / span if span else 0.0

    def projected(sample: dict) -> tuple[float, float]:
        east = (sample["lon"] - frame[1]) * 111_320.0 * math.cos(math.radians(frame[0]))
        north = (sample["lat"] - frame[0]) * 110_540.0
        return east, north

    first = projected(samples[index])
    if ratio == 0.0:
        return first
    second = projected(samples[index + 1])
    return first[0] + (second[0] - first[0]) * ratio, first[1] + (second[1] - first[1]) * ratio


def lap_translation(samples: list[dict], laps: dict[int, tuple], reference: int, comparison: int) -> dict:
    reference_view = lap_view(samples, laps[reference])
    comparison_view = lap_view(samples, laps[comparison])
    frame = (samples[reference_view[0]]["lat"], samples[reference_view[0]]["lon"])
    offsets: list[tuple[float, float]] = []
    for point in range(2, 99):
        ref = point_at_progress(samples, reference_view, point / 100.0, frame)
        cmp = point_at_progress(samples, comparison_view, point / 100.0, frame)
        offsets.append((ref[0] - cmp[0], ref[1] - cmp[1]))
    east = statistics.median(value[0] for value in offsets)
    north = statistics.median(value[1] for value in offsets)
    residual = math.sqrt(sum((x - east) ** 2 + (y - north) ** 2 for x, y in offsets) / len(offsets))
    return {
        "east_m": east,
        "north_m": north,
        "magnitude_m": math.hypot(east, north),
        "residual_rms_m": residual,
    }


def session_audit(vbo: Path) -> tuple[dict, list[dict], list[tuple]]:
    csv_path = matching_csv(vbo)
    if csv_path is None:
        raise FileNotFoundError(f"No matching RaceBox CSV for {vbo}")
    samples, start_line = read_vbo(vbo)
    metadata_lines, csv_samples = racebox_header_offset(csv_path)
    blocks = stationary_blocks(samples)
    all_crossings, accepted = directed_crossings(samples, start_line)
    if len(accepted) < 2:
        raise RuntimeError(f"Fewer than two accepted forward crossings in {vbo}")

    # These are session-local landmark candidates, not shared control points.
    # A qualifying run can start anywhere and a race can start from any grid box.
    before_first_crossing = [block for block in blocks if block[1] < accepted[0][0]]
    between_first_two_crossings = [
        block for block in blocks if block[0] > accepted[0][0] and block[1] < accepted[1][0]
    ]
    after_final_crossing = [block for block in blocks if block[0] > accepted[-1][0]]
    place_down_block = before_first_crossing[0] if before_first_crossing else None
    qualifying_start_block = before_first_crossing[-1] if before_first_crossing else None
    race_grid_start_block = between_first_two_crossings[-1] if between_first_two_crossings else None
    race_stop_block = after_final_crossing[-1] if after_final_crossing else None
    place_down = zone_summary(samples, *place_down_block) if place_down_block else None
    qualifying_start = zone_summary(samples, *qualifying_start_block) if qualifying_start_block else None
    race_grid_start = zone_summary(samples, *race_grid_start_block) if race_grid_start_block else None
    race_stop = zone_summary(samples, *race_stop_block) if race_stop_block else None
    session_type = known_session_type(vbo)
    selected_start = race_grid_start if session_type == "race" else None
    if session_type == "race":
        selected_start_reason = (
            "user identified this as the final session of the day and therefore the race; back-straight grid candidate selected"
        )
    elif session_type == "qualifying_or_practice":
        selected_start_reason = "not selected until qualifying and practice are distinguished"
    else:
        selected_start_reason = "not selected because the session type is unknown"

    # The current native implementation uses the first qualifying stationary block
    # even when it occurs later in the recording. Disclose that distinction.
    zero_begin = blocks[0][0]
    zero_end = zero_begin + 49
    zero_axes = {
        name: statistics.median(samples[index][name] for index in range(zero_begin, zero_end + 1))
        for name in ("long_g", "lat_g", "vertical_g", "gyro_x", "gyro_y", "gyro_z")
    }
    line_center = {
        "lat": (start_line[0][0] + start_line[1][0]) * 0.5,
        "lon": (start_line[0][1] + start_line[1][1]) * 0.5,
    }
    result = {
        "session": vbo.name,
        "year": 2025 if "2025" in vbo.name else 2026,
        "samples": len(samples),
        "csv_samples": csv_samples,
        "metadata_lines_before_csv_header": metadata_lines,
        "native_raw_csv_compatible": metadata_lines == 0,
        "raw_crossings": len(all_crossings),
        "accepted_crossings": len(accepted),
        "session_type": session_type,
        "selected_start": selected_start,
        "selected_start_reason": selected_start_reason,
        "place_down": place_down,
        "app_zero_2s": zone_summary(samples, zero_begin, zero_end),
        "qualifying_start_candidate": qualifying_start,
        "race_grid_start_candidate": race_grid_start,
        "race_stop": race_stop,
        "landmark_detection": {
            "place_down_detected": place_down is not None,
            "qualifying_start_candidate_detected": qualifying_start is not None,
            "race_grid_start_candidate_detected": race_grid_start is not None,
            "race_stop_detected": race_stop is not None,
            "qualifying_start_same_block_as_place_down": place_down_block is not None and qualifying_start_block == place_down_block,
            "imu_zero_uses_place_down": place_down_block is not None and blocks[0] == place_down_block,
            "imu_zero_source": "place_down" if place_down_block is not None and blocks[0] == place_down_block else "later_stationary_fallback",
        },
        "imu_zero": zero_axes,
        "start_line_center": line_center,
    }
    return result, samples, accepted


def run_audit() -> dict:
    sessions: list[dict] = []
    golden_samples: list[dict] | None = None
    golden_crossings: list[tuple] | None = None
    for vbo in unique_vbo_files():
        csv_path = matching_csv(vbo)
        if csv_path is None:
            continue
        session, samples, crossings = session_audit(vbo)
        sessions.append(session)
        if "14-49" in vbo.name:
            golden_samples, golden_crossings = samples, crossings

    if golden_samples is None or golden_crossings is None:
        raise RuntimeError("The July 11 14:49 golden session was not found")
    laps = {
        index + 1: (crossing[0], golden_crossings[index + 1][0] - 1 if index + 1 < len(golden_crossings) else len(golden_samples) - 1)
        for index, crossing in enumerate(golden_crossings)
    }
    translations = {
        raw_lap: lap_translation(golden_samples, laps, 17, raw_lap)
        for raw_lap in range(2, len(golden_crossings))
        if raw_lap != 17
    }
    return {
        "sessions": sessions,
        "raw17_reference_translations": translations,
        "raw7_neighbor_checks": {
            "raw6_to_raw7": lap_translation(golden_samples, laps, 6, 7),
            "raw8_to_raw7": lap_translation(golden_samples, laps, 8, 7),
        },
    }


def compact_summary(result: dict) -> dict:
    return {
        "unique_sessions": len(result["sessions"]),
        "sessions": [
            {
                "name": session["session"],
                "samples": session["samples"],
                "accepted_crossings": session["accepted_crossings"],
                "metadata_lines": session["metadata_lines_before_csv_header"],
                "session_type": session["session_type"],
                "selected_start": session["selected_start"],
                "selected_start_reason": session["selected_start_reason"],
                "landmark_detection": session["landmark_detection"],
                "place_down": session["place_down"],
                "qualifying_start_candidate": session["qualifying_start_candidate"],
                "race_grid_start_candidate": session["race_grid_start_candidate"],
                "race_stop": session["race_stop"],
                "app_zero_2s": session["app_zero_2s"],
                "imu_zero": session["imu_zero"],
            }
            for session in result["sessions"]
        ],
        "raw7_vs_raw17": result["raw17_reference_translations"][7],
        "raw7_neighbor_checks": result["raw7_neighbor_checks"],
    }


if __name__ == "__main__":
    print(json.dumps(compact_summary(run_audit()), indent=2))
