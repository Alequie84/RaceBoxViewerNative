#!/usr/bin/env python3
"""Deterministic, dependency-free RaceBox previous/current telemetry analytics."""

from __future__ import annotations

import csv
import copy
import io
import math
import re
import statistics
from collections import defaultdict
from datetime import datetime, timezone
from typing import Any, Iterable


CSV_CONTRACT = "racebox-session-analytics-csv-v1"
ANALYTICS_CONTRACT = "racebox-setup-analytics-v3"
SESSION_REVIEW_CONTRACT = "racebox-session-review-v5"
STANDARD_GRAVITY = 9.80665
MAX_CSV_CHARS = 6_000_000
MAX_ROWS = 120_000
SESSION_PROGRESS_BINS = 100

LAP_NUMBER_WORDS = {
    "one": 1,
    "two": 2,
    "three": 3,
    "four": 4,
    "five": 5,
    "six": 6,
    "seven": 7,
    "eight": 8,
    "nine": 9,
    "ten": 10,
    "eleven": 11,
    "twelve": 12,
    "thirteen": 13,
    "fourteen": 14,
    "fifteen": 15,
    "sixteen": 16,
    "seventeen": 17,
    "eighteen": 18,
    "nineteen": 19,
    "twenty": 20,
}


def _number(value: Any) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def _median(values: Iterable[float]) -> float | None:
    values = list(values)
    return statistics.median(values) if values else None


def _quantile(values: Iterable[float], fraction: float) -> float | None:
    ordered = sorted(values)
    if not ordered:
        return None
    position = max(0.0, min(1.0, fraction)) * (len(ordered) - 1)
    low = int(math.floor(position))
    high = int(math.ceil(position))
    if low == high:
        return ordered[low]
    weight = position - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def _rounded(value: float | None, digits: int = 4) -> float | None:
    return round(value, digits) if value is not None and math.isfinite(value) else None


def _lap_number(value: str) -> int | None:
    token = value.strip().lower()
    if token.isdigit():
        return int(token)
    return LAP_NUMBER_WORDS.get(token)


def requested_lap_scope(
    question: str,
    history: list[dict[str, Any]] | None,
    lap_table: list[dict[str, Any]],
) -> dict[str, Any]:
    """Resolve driver-facing race-lap scope without confusing turn numbers for laps."""

    available = {
        int(item["race_lap"]): int(item["raw_lap"])
        for item in lap_table
        if item.get("phase") == "complete"
        and int(item.get("race_lap") or 0) > 0
        and item.get("raw_lap") is not None
    }
    all_race_laps = sorted(available)
    all_raw_laps = [available[value] for value in all_race_laps]
    sources: list[tuple[str, str]] = [("current_question", str(question or ""))]
    for item in reversed(history or []):
        if not isinstance(item, dict) or item.get("role") != "user":
            continue
        content = item.get("content")
        if isinstance(content, str) and content.strip():
            sources.append(("recent_driver_context", content))

    number = r"(?:\d{1,2}|" + "|".join(LAP_NUMBER_WORDS) + r")"
    selected_race_laps: list[int] | None = None
    source_name = "all_complete_laps"
    description = "all complete race laps"
    matched_text = ""
    for name, supplied in sources:
        text = supplied.lower()
        if not text:
            continue
        if re.search(r"\b(?:all|every)\s+(?:complete\s+|race\s+)?laps?\b", text):
            selected_race_laps = all_race_laps
            source_name = name
            description = "all complete race laps"
            matched_text = "all laps"
            break

        match = re.search(rf"\bfirst\s+({number})\s+(?:complete\s+|race\s+)?laps?\b", text)
        if match:
            limit = _lap_number(match.group(1))
            if limit is not None:
                selected_race_laps = [value for value in all_race_laps if 1 <= value <= limit]
                source_name = name
                description = f"race laps 1-{limit}"
                matched_text = match.group(0)
                break

        match = re.search(rf"\bbefore\s+(?:the\s+)?(?:crash\s+(?:on|at)\s+)?lap\s+({number})\b", text)
        if match:
            limit = _lap_number(match.group(1))
            if limit is not None:
                selected_race_laps = [value for value in all_race_laps if value < limit]
                source_name = name
                description = f"race laps before lap {limit}"
                matched_text = match.group(0)
                break

        match = re.search(
            rf"\blaps?\s+({number})\s*(?:-|through|thru|to)\s*({number})\b",
            text,
        )
        if match:
            start = _lap_number(match.group(1))
            end = _lap_number(match.group(2))
            if start is not None and end is not None:
                low, high = sorted((start, end))
                selected_race_laps = [value for value in all_race_laps if low <= value <= high]
                source_name = name
                description = f"race laps {low}-{high}"
                matched_text = match.group(0)
                break

        match = re.search(rf"\blast\s+({number})\s+(?:complete\s+|race\s+)?laps?\b", text)
        if match:
            count = _lap_number(match.group(1))
            if count is not None:
                selected_race_laps = all_race_laps[-count:]
                source_name = name
                description = f"last {count} complete race laps"
                matched_text = match.group(0)
                break

    if not selected_race_laps or len(selected_race_laps) < 2:
        selected_race_laps = all_race_laps
        source_name = "all_complete_laps"
        description = "all complete race laps"
        matched_text = ""

    reference_race_lap = None
    current_text = str(question or "").lower()
    single = re.search(rf"\b(?:race\s+)?lap\s+({number})\b", current_text)
    if single:
        supplied_reference = _lap_number(single.group(1))
        if supplied_reference in selected_race_laps:
            reference_race_lap = supplied_reference

    return {
        "status": "requested" if source_name != "all_complete_laps" else "all_complete_laps",
        "source": source_name,
        "description": description,
        "matched_driver_words": matched_text,
        "available_race_laps": all_race_laps,
        "selected_race_laps": selected_race_laps,
        "selected_raw_laps": [available[value] for value in selected_race_laps],
        "reference_race_lap": reference_race_lap,
        "reference_raw_lap": available.get(reference_race_lap),
        "selection_applied": selected_race_laps != all_race_laps,
        "comparison_lap_count": len(selected_race_laps),
        "unavailable_requested_laps": [],
    }


def parse_analytics_csv(text: str) -> list[dict[str, Any]]:
    if not isinstance(text, str) or not text:
        raise ValueError("analytics_csv is required")
    if len(text) > MAX_CSV_CHARS:
        raise ValueError("analytics_csv is too large")
    lines = text.splitlines()
    if not lines or lines[0].strip() != f"#contract,{CSV_CONTRACT}":
        raise ValueError("unsupported analytics CSV contract")
    data_lines = [line for line in lines if line and not line.startswith("#")]
    if not data_lines:
        raise ValueError("analytics CSV has no header")

    required = {
        "time_s",
        "lap_raw",
        "lap_phase",
        "lap_elapsed_s",
        "lap_progress_percent",
        "latitude",
        "longitude",
        "speed_kmh",
        "lateral_g",
        "longitudinal_g",
        "steering_percent",
        "throttle_percent",
        "brake_percent",
        "satellites",
    }
    reader = csv.DictReader(io.StringIO("\n".join(data_lines)))
    if not reader.fieldnames or not required.issubset(reader.fieldnames):
        raise ValueError("analytics CSV is missing required columns")

    rows: list[dict[str, Any]] = []
    for raw in reader:
        if len(rows) >= MAX_ROWS:
            raise ValueError("analytics CSV has too many rows")
        time_s = _number(raw.get("time_s"))
        speed = _number(raw.get("speed_kmh"))
        lateral = _number(raw.get("lateral_g"))
        longitudinal = _number(raw.get("longitudinal_g"))
        if time_s is None or speed is None or lateral is None or longitudinal is None:
            continue
        absolute_time_us = _number(raw.get("absolute_time_us"))
        rows.append(
            {
                "time": time_s,
                "absolute_time_us": (
                    int(absolute_time_us) if absolute_time_us is not None else None
                ),
                "lap": int(_number(raw.get("lap_raw")) or 0),
                "race_lap": int(_number(raw.get("lap_race")) or 0),
                "phase": str(raw.get("lap_phase") or ""),
                "lap_elapsed": _number(raw.get("lap_elapsed_s")) or 0.0,
                "progress": _number(raw.get("lap_progress_percent")) or 0.0,
                "latitude": _number(raw.get("latitude")),
                "longitude": _number(raw.get("longitude")),
                "speed": speed,
                "lateral": lateral,
                "longitudinal": longitudinal,
                "vertical": _number(raw.get("vertical_g")),
                "raw_lateral": _number(raw.get("raw_lateral_g")),
                "raw_vertical": _number(raw.get("raw_vertical_g")),
                "yaw": _number(raw.get("yaw_rate_dps")),
                "steering": _number(raw.get("steering_percent")),
                "throttle": _number(raw.get("throttle_percent")),
                "brake": _number(raw.get("brake_percent")),
                "satellites": _number(raw.get("satellites")),
            }
        )
    if len(rows) < 10:
        raise ValueError("analytics CSV has too few usable rows")
    rows.sort(key=lambda row: row["time"])
    return rows


def _complete_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [row for row in rows if row["phase"] == "complete"]


def _lap_groups(rows: list[dict[str, Any]]) -> dict[int, list[dict[str, Any]]]:
    result: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for row in _complete_rows(rows):
        result[row["lap"]].append(row)
    return dict(result)


def _lap_summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    groups = _lap_groups(rows)
    durations = [
        max(row["lap_elapsed"] for row in group)
        for group in groups.values()
        if len(group) >= 5
    ]
    durations.sort()
    top = durations[: min(3, len(durations))]
    return {
        "complete_laps": len(durations),
        "best_s": _rounded(min(durations) if durations else None),
        "top3_median_s": _rounded(_median(top)),
        "all_lap_median_s": _rounded(_median(durations)),
        "top3_range_s": _rounded(max(top) - min(top) if len(top) >= 2 else None),
        "lap_times_s": [_rounded(value) for value in durations[:20]],
    }


def _sample_quality(rows: list[dict[str, Any]]) -> dict[str, Any]:
    deltas = [
        current["time"] - previous["time"]
        for previous, current in zip(rows, rows[1:])
        if 0.0 < current["time"] - previous["time"] < 1.0
    ]
    complete = _complete_rows(rows)
    radio_rows = [
        row for row in complete
        if row["steering"] is not None
        and row["throttle"] is not None
        and row["brake"] is not None
    ]
    satellites = [row["satellites"] for row in complete if row["satellites"] is not None]
    return {
        "rows": len(rows),
        "complete_rows": len(complete),
        "median_sample_period_ms": _rounded((_median(deltas) or 0.0) * 1000.0, 2),
        "radio_coverage": _rounded(len(radio_rows) / max(1, len(complete)), 3),
        "median_satellites": _rounded(_median(satellites), 1),
    }


def _single_session_quality_confidence(
    laps: dict[str, Any], quality: dict[str, Any]
) -> int:
    """Score whether one run is suitable for descriptive, not causal, review."""

    score = 20
    score += min(30, int(laps.get("complete_laps") or 0) * 6)
    score += int(20 * float(quality.get("radio_coverage") or 0.0))
    period = quality.get("median_sample_period_ms")
    if period is not None and period <= 50.0:
        score += 15
    elif period is not None and period <= 100.0:
        score += 8
    satellites = quality.get("median_satellites")
    if satellites is not None and satellites >= 10:
        score += 10
    return max(0, min(100, score))


def _session_lap_table(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[tuple[int, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        groups[(row["lap"], row["phase"])].append(row)

    table: list[dict[str, Any]] = []
    for (lap, phase), group in groups.items():
        if len(group) < 2:
            continue
        group.sort(key=lambda row: row["time"])
        duration = max(row["lap_elapsed"] for row in group)
        steering = [abs(row["steering"]) for row in group if row["steering"] is not None]
        throttle = [row["throttle"] for row in group if row["throttle"] is not None]
        brake = [row["brake"] for row in group if row["brake"] is not None]
        yaw = [abs(row["yaw"]) for row in group if row["yaw"] is not None]
        lateral = [abs(row["lateral"]) for row in group]
        longitudinal = [row["longitudinal"] for row in group]
        response_samples = [
            abs(row["lateral"]) / max(8.0, abs(row["steering"])) * 50.0
            for row in group
            if row["steering"] is not None
            and abs(row["steering"]) >= 8.0
            and row["speed"] >= 8.0
            and (row["brake"] is None or row["brake"] <= 5.0)
        ]
        braking = _braking_analysis(group) if phase == "complete" else {}
        table.append(
            {
                "raw_lap": lap,
                "race_lap": int(_median(row["race_lap"] for row in group) or 0),
                "phase": phase,
                "start_time_s": _rounded(group[0]["time"]),
                "end_time_s": _rounded(group[-1]["time"]),
                "duration_s": _rounded(duration),
                "sample_count": len(group),
                "average_speed_kmh": _rounded(
                    sum(row["speed"] for row in group) / len(group)
                ),
                "maximum_speed_kmh": _rounded(max(row["speed"] for row in group)),
                "minimum_speed_kmh": _rounded(min(row["speed"] for row in group)),
                "p90_abs_lateral_g": _rounded(_quantile(lateral, 0.90)),
                "p90_acceleration_g": _rounded(_quantile(longitudinal, 0.90)),
                "p10_braking_g": _rounded(_quantile(longitudinal, 0.10)),
                "p90_abs_steering_percent": _rounded(_quantile(steering, 0.90)),
                "p90_abs_yaw_rate_dps": _rounded(_quantile(yaw, 0.90)),
                "full_throttle_sample_percent": _rounded(
                    100.0 * sum(value >= 90.0 for value in throttle) / len(throttle)
                    if throttle else None
                ),
                "brake_active_sample_percent": _rounded(
                    100.0 * sum(value >= 10.0 for value in brake) / len(brake)
                    if brake else None
                ),
                "lateral_response_g_at_50pct_steering_proxy": _rounded(
                    _median(response_samples)
                ),
                "possible_brake_lockup_or_low_grip_indicators": braking.get(
                    "possible_lockup_or_low_grip_indicators"
                ),
                "radio_coverage": _rounded(
                    sum(
                        row["steering"] is not None
                        and row["throttle"] is not None
                        and row["brake"] is not None
                        for row in group
                    )
                    / len(group),
                    3,
                ),
            }
        )
    table.sort(key=lambda item: item["start_time_s"] or 0.0)
    return table[:100]


def _session_trend(complete_laps: list[dict[str, Any]]) -> dict[str, Any]:
    if len(complete_laps) < 4:
        return {
            "status": "data_limited",
            "minimum_complete_laps": 4,
            "interpretation": (
                "At least four complete laps are needed before early-versus-late run changes are described."
            ),
        }
    group_size = max(1, len(complete_laps) // 3)
    early = complete_laps[:group_size]
    late = complete_laps[-group_size:]

    def median_field(group: list[dict[str, Any]], key: str) -> float | None:
        return _median(
            item[key] for item in group if item.get(key) is not None
        )

    def delta(key: str) -> float | None:
        before = median_field(early, key)
        after = median_field(late, key)
        return after - before if before is not None and after is not None else None

    return {
        "status": "measured",
        "early_laps": [item["raw_lap"] for item in early],
        "late_laps": [item["raw_lap"] for item in late],
        "late_minus_early_lap_time_s": _rounded(delta("duration_s")),
        "late_minus_early_average_speed_kmh": _rounded(delta("average_speed_kmh")),
        "late_minus_early_maximum_speed_kmh": _rounded(delta("maximum_speed_kmh")),
        "late_minus_early_p90_abs_lateral_g": _rounded(delta("p90_abs_lateral_g")),
        "late_minus_early_p90_abs_steering_percent": _rounded(
            delta("p90_abs_steering_percent")
        ),
        "late_minus_early_full_throttle_sample_percent": _rounded(
            delta("full_throttle_sample_percent")
        ),
        "late_minus_early_lateral_response_proxy_g": _rounded(
            delta("lateral_response_g_at_50pct_steering_proxy")
        ),
        "interpretation": (
            "These are descriptive changes from the first third to the final third of the complete laps. "
            "They can show fade, improvement, traffic, battery, tire, or driver trends, but do not identify the cause by themselves."
        ),
    }


def _nearest_progress_speed(
    rows: list[dict[str, Any]], progress: float
) -> float | None:
    nearby = [row for row in rows if abs(row["progress"] - progress) <= 1.5]
    if not nearby:
        return None
    nearest = min(nearby, key=lambda row: abs(row["progress"] - progress))
    return nearest["speed"]


def _absolute_time_utc(absolute_time_us: int | None) -> str | None:
    if absolute_time_us is None or absolute_time_us <= 0:
        return None
    try:
        return (
            datetime.fromtimestamp(absolute_time_us / 1_000_000.0, tz=timezone.utc)
            .isoformat(timespec="milliseconds")
            .replace("+00:00", "Z")
        )
    except (OverflowError, OSError, ValueError):
        return None


def _incident_candidates(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Find unusual abrupt near-stops without claiming that they were crashes."""

    groups = _lap_groups(rows)
    candidates: list[dict[str, Any]] = []
    for lap, unsorted_group in groups.items():
        group = sorted(unsorted_group, key=lambda row: row["time"])
        sample_periods = [
            current["time"] - previous["time"]
            for previous, current in zip(group, group[1:])
            if 0.0 < current["time"] - previous["time"] <= 0.250
        ]
        sample_period = _median(sample_periods) or 0.04
        index = 0
        while index < len(group):
            if group[index]["speed"] > 10.0:
                index += 1
                continue
            start = index
            end = index
            while (
                end + 1 < len(group)
                and group[end + 1]["speed"] <= 10.0
                and 0.0 < group[end + 1]["time"] - group[end]["time"] <= 0.250
            ):
                end += 1
            index = end + 1

            low_duration = group[end]["time"] - group[start]["time"] + sample_period
            if low_duration < 0.080:
                continue
            minimum_index = min(
                range(start, end + 1), key=lambda item: group[item]["speed"]
            )
            minimum = group[minimum_index]
            prior = [
                row
                for row in group
                if 0.0 < group[start]["time"] - row["time"] <= 0.320
            ]
            if not prior:
                continue
            speed_before_row = max(prior, key=lambda row: row["speed"])
            speed_drop = speed_before_row["speed"] - minimum["speed"]
            if speed_before_row["speed"] < 15.0 or speed_drop < 12.0:
                continue

            reference_speeds = []
            for other_lap, other_group in groups.items():
                if other_lap == lap:
                    continue
                reference_speed = _nearest_progress_speed(
                    other_group, minimum["progress"]
                )
                if reference_speed is not None:
                    reference_speeds.append(reference_speed)
            same_progress_speed = _median(reference_speeds)
            same_progress_deficit = (
                same_progress_speed - minimum["speed"]
                if same_progress_speed is not None
                else None
            )
            if (
                len(reference_speeds) >= 2
                and (same_progress_deficit is None or same_progress_deficit < 8.0)
            ):
                continue
            if len(reference_speeds) < 2 and not (
                minimum["speed"] <= 5.0
                and speed_drop >= 18.0
                and low_duration >= 0.120
            ):
                continue

            score = 20
            score += 20 if minimum["speed"] <= 5.0 else 15
            score += 25 if speed_drop >= 18.0 else 20 if speed_drop >= 15.0 else 15
            if same_progress_deficit is not None:
                score += 25 if same_progress_deficit >= 12.0 else 15
            else:
                score += 5
            score += 10 if low_duration >= 0.120 else 5
            score = min(95, score)

            window = [
                row
                for row in group
                if minimum["time"] - 0.320 <= row["time"] <= minimum["time"] + 0.320
            ]
            event_id = f"incident:raw-lap-{lap}:time-{int(round(minimum['time'] * 1000.0))}ms"
            candidates.append(
                {
                    "evidence_id": event_id,
                    "classification": "possible_incident_or_unusual_stop",
                    "confidence": score,
                    "confidence_label": "high" if score >= 80 else "medium",
                    "raw_lap": lap,
                    "race_lap": minimum.get("race_lap") or None,
                    "session_time_s": _rounded(minimum["time"], 3),
                    "absolute_time_us": minimum.get("absolute_time_us"),
                    "absolute_time_utc": _absolute_time_utc(
                        minimum.get("absolute_time_us")
                    ),
                    "lap_elapsed_s": _rounded(minimum["lap_elapsed"], 3),
                    "lap_progress_percent": _rounded(minimum["progress"], 2),
                    "speed_before_kmh": _rounded(speed_before_row["speed"], 2),
                    "minimum_speed_kmh": _rounded(minimum["speed"], 2),
                    "speed_drop_kmh": _rounded(speed_drop, 2),
                    "speed_drop_window_s": _rounded(
                        minimum["time"] - speed_before_row["time"], 3
                    ),
                    "duration_at_or_below_10_kmh_s": _rounded(low_duration, 3),
                    "same_progress_median_speed_other_laps_kmh": _rounded(
                        same_progress_speed, 2
                    ),
                    "speed_deficit_vs_other_laps_kmh": _rounded(
                        same_progress_deficit, 2
                    ),
                    "same_progress_reference_laps": len(reference_speeds),
                    "maximum_abs_lateral_g_near_event": _rounded(
                        max((abs(row["lateral"]) for row in window), default=0.0), 3
                    ),
                    "maximum_abs_longitudinal_g_near_event": _rounded(
                        max((abs(row["longitudinal"]) for row in window), default=0.0),
                        3,
                    ),
                    "maximum_abs_yaw_rate_dps_near_event": _rounded(
                        max(
                            (
                                abs(row["yaw"])
                                for row in window
                                if row["yaw"] is not None
                            ),
                            default=0.0,
                        ),
                        2,
                    ),
                    "interpretation": (
                        "This was a one-off abrupt speed loss and near-stop relative to the other laps at the same track position. "
                        "It can be contact, a spin or flip, marshalling, traffic, or an intentional stop; telemetry alone does not prove a crash."
                    ),
                }
            )
    candidates.sort(key=lambda item: item["session_time_s"] or 0.0)
    return candidates[:8]


def _incident_consequence(
    lap_table: list[dict[str, Any]], candidate: dict[str, Any]
) -> dict[str, Any]:
    """Compare the incident lap and every later complete lap with pre-event pace."""

    complete = [lap for lap in lap_table if lap["phase"] == "complete"]
    impact_index = next(
        (
            index
            for index, lap in enumerate(complete)
            if lap["raw_lap"] == candidate["raw_lap"]
        ),
        None,
    )
    if impact_index is None:
        return {"status": "data_limited", "reason": "impact lap was not complete"}
    before = complete[:impact_index]
    impact = complete[impact_index]
    after = complete[impact_index + 1 :]
    baseline_laps = before[-6:]
    baseline_durations = [
        lap["duration_s"] for lap in baseline_laps if lap.get("duration_s") is not None
    ]
    if len(baseline_durations) < 3:
        return {
            "status": "data_limited",
            "reason": "at least three complete pre-event laps are required",
            "pre_event_complete_laps": len(baseline_durations),
            "impact_raw_lap": candidate["raw_lap"],
        }
    baseline_pace = _median(baseline_durations)
    impact_duration = impact.get("duration_s")
    after_durations = [
        lap["duration_s"] for lap in after if lap.get("duration_s") is not None
    ]
    if baseline_pace is None or impact_duration is None:
        return {"status": "data_limited", "reason": "lap duration is unavailable"}

    impact_delta = impact_duration - baseline_pace
    post_delta = sum(after_durations) - baseline_pace * len(after_durations)
    actual_remaining = impact_duration + sum(after_durations)
    expected_remaining = baseline_pace * (1 + len(after_durations))
    total_delta = actual_remaining - expected_remaining

    def median_field(items: list[dict[str, Any]], key: str) -> float | None:
        return _median(item[key] for item in items if item.get(key) is not None)

    def post_minus_pre(key: str) -> float | None:
        prior = median_field(baseline_laps, key)
        later = median_field(after, key)
        return later - prior if prior is not None and later is not None else None

    return {
        "status": "measured",
        "association": "possible_incident_associated_not_causal_proof",
        "primary_candidate_id": candidate["evidence_id"],
        "scope": "impact lap plus every later complete lap through the end of the run",
        "baseline_raw_laps": [lap["raw_lap"] for lap in baseline_laps],
        "baseline_complete_laps": len(baseline_laps),
        "baseline_pace_median_s": _rounded(baseline_pace, 3),
        "impact_raw_lap": impact["raw_lap"],
        "impact_race_lap": impact.get("race_lap") or None,
        "impact_lap_time_s": _rounded(impact_duration, 3),
        "impact_lap_delta_vs_pre_event_pace_s": _rounded(impact_delta, 3),
        "post_event_raw_laps": [lap["raw_lap"] for lap in after],
        "post_event_complete_laps": len(after_durations),
        "post_event_pace_median_s": _rounded(_median(after_durations), 3),
        "post_event_cumulative_delta_vs_pre_event_pace_s": _rounded(post_delta, 3),
        "actual_impact_through_finish_time_s": _rounded(actual_remaining, 3),
        "projected_impact_through_finish_at_pre_event_pace_s": _rounded(
            expected_remaining, 3
        ),
        "total_impact_through_finish_delta_s": _rounded(total_delta, 3),
        "estimated_time_lost_s": _rounded(max(0.0, total_delta), 3),
        "post_minus_pre_median_average_speed_kmh": _rounded(
            post_minus_pre("average_speed_kmh"), 3
        ),
        "post_minus_pre_median_maximum_speed_kmh": _rounded(
            post_minus_pre("maximum_speed_kmh"), 3
        ),
        "post_minus_pre_median_p90_abs_lateral_g": _rounded(
            post_minus_pre("p90_abs_lateral_g"), 4
        ),
        "post_minus_pre_median_p90_abs_yaw_rate_dps": _rounded(
            post_minus_pre("p90_abs_yaw_rate_dps"), 3
        ),
        "post_minus_pre_median_p90_abs_steering_percent": _rounded(
            post_minus_pre("p90_abs_steering_percent"), 3
        ),
        "interpretation": (
            "The projection holds the median of the last six available complete pre-event laps constant and compares it with the incident lap plus every later complete lap. "
            "Traffic, battery, tires, track evolution, damage, and driver adaptation can also contribute, so this is time associated with the change point rather than proof of crash damage."
        ),
    }


def _binned(rows: list[dict[str, Any]], mode: str) -> dict[tuple[int, ...], list[float]]:
    bins: dict[tuple[int, ...], list[float]] = defaultdict(list)
    if mode == "lateral":
        for row in _complete_rows(rows):
            steering = row["steering"]
            brake = row["brake"]
            if (
                steering is None
                or brake is None
                or row["speed"] < 8.0
                or abs(steering) < 8.0
                or brake > 5.0
            ):
                continue
            key = (
                int(max(0.0, min(99.999, row["progress"])) // 2.0),
                int(row["speed"] // 3.0),
                int(abs(steering) // 5.0),
            )
            bins[key].append(abs(row["lateral"]))
    return dict(bins)


def _matched_cell_delta(
    previous: dict[tuple[int, ...], list[float]],
    current: dict[tuple[int, ...], list[float]],
    minimum_per_cell: int = 3,
) -> tuple[float | None, int, int]:
    weighted_sum = 0.0
    total_weight = 0
    shared_bins = 0
    for key in sorted(previous.keys() & current.keys()):
        before = previous[key]
        after = current[key]
        if len(before) < minimum_per_cell or len(after) < minimum_per_cell:
            continue
        weight = min(len(before), len(after), 100)
        weighted_sum += (statistics.median(after) - statistics.median(before)) * weight
        total_weight += weight
        shared_bins += 1
    return (
        weighted_sum / total_weight if total_weight else None,
        shared_bins,
        total_weight,
    )


def _lateral_analysis(
    previous: list[dict[str, Any]], current: list[dict[str, Any]]
) -> dict[str, Any]:
    before_bins = _binned(previous, "lateral")
    after_bins = _binned(current, "lateral")
    delta, shared_bins, matched_samples = _matched_cell_delta(before_bins, after_bins)
    before_complete = _complete_rows(previous)
    after_complete = _complete_rows(current)
    return {
        "status": "measured" if shared_bins >= 10 and matched_samples >= 40 else "data_limited",
        "matched_lateral_response_delta_g": _rounded(delta),
        "matched_speed_bin_kmh": 3.0,
        "matched_abs_steering_bin_percent": 5.0,
        "matched_lap_progress_bin_percent": 2.0,
        "shared_bins": shared_bins,
        "matched_sample_weight": matched_samples,
        "previous_p95_abs_lateral_g": _rounded(
            _quantile((abs(row["lateral"]) for row in before_complete), 0.95)
        ),
        "current_p95_abs_lateral_g": _rounded(
            _quantile((abs(row["lateral"]) for row in after_complete), 0.95)
        ),
        "evidence_id": "dynamics:lateral:matched-speed-steering",
        "interpretation": (
            "Positive means more chassis lateral response at matched speed and steering input. "
            "It is not direct tire load or proof of more grip."
        ),
    }


def _derived_acceleration(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for previous, current in zip(rows, rows[1:]):
        dt = current["time"] - previous["time"]
        if (
            previous["phase"] != "complete"
            or current["phase"] != "complete"
            or previous["lap"] != current["lap"]
            or dt < 0.015
            or dt > 0.250
        ):
            continue
        output.append(
            {
                **current,
                "speed_accel_g": ((current["speed"] - previous["speed"]) / 3.6) / dt / STANDARD_GRAVITY,
            }
        )
    return output


def _forward_bins(rows: list[dict[str, Any]]) -> dict[tuple[int, ...], list[float]]:
    bins: dict[tuple[int, ...], list[float]] = defaultdict(list)
    for row in _derived_acceleration(rows):
        steering = row["steering"]
        throttle = row["throttle"]
        brake = row["brake"]
        if (
            steering is None
            or throttle is None
            or brake is None
            or throttle < 90.0
            or brake > 5.0
            or abs(steering) > 20.0
            or row["speed"] < 5.0
        ):
            continue
        key = (
            int(max(0.0, min(99.999, row["progress"])) // 4.0),
            int(row["speed"] // 5.0),
            int(abs(steering) // 10.0),
        )
        bins[key].append(row["speed_accel_g"])
    return dict(bins)


def _forward_analysis(
    previous: list[dict[str, Any]], current: list[dict[str, Any]]
) -> dict[str, Any]:
    before_bins = _forward_bins(previous)
    after_bins = _forward_bins(current)
    delta, shared_bins, matched_samples = _matched_cell_delta(before_bins, after_bins)
    before_values = [value for values in before_bins.values() for value in values]
    after_values = [value for values in after_bins.values() for value in values]
    return {
        "status": "measured" if shared_bins >= 5 and matched_samples >= 25 else "data_limited",
        "matched_speed_derived_acceleration_delta_g": _rounded(delta),
        "matched_speed_bin_kmh": 5.0,
        "matched_abs_steering_bin_percent": 10.0,
        "matched_lap_progress_bin_percent": 4.0,
        "shared_bins": shared_bins,
        "matched_sample_weight": matched_samples,
        "previous_p90_speed_derived_acceleration_g": _rounded(_quantile(before_values, 0.90)),
        "current_p90_speed_derived_acceleration_g": _rounded(_quantile(after_values, 0.90)),
        "evidence_id": "dynamics:forward-bite:matched-full-throttle",
        "interpretation": (
            "Positive means stronger speed gain at full throttle in matched speed/steering bins. "
            "Battery, line, surface, tire state, and gearing can still cause the difference."
        ),
    }


def _best_straights(rows: list[dict[str, Any]]) -> list[dict[str, float]]:
    results: list[dict[str, float]] = []
    for group in _lap_groups(rows).values():
        best: tuple[float, int, int] | None = None
        start: int | None = None
        for index, row in enumerate(group):
            straight = (
                row["throttle"] is not None
                and row["brake"] is not None
                and row["steering"] is not None
                and row["throttle"] >= 80.0
                and row["brake"] <= 5.0
                and abs(row["steering"]) <= 35.0
            )
            if straight and start is None:
                start = index
            if (not straight or index == len(group) - 1) and start is not None:
                end = index if straight else index - 1
                if end > start:
                    duration = group[end]["time"] - group[start]["time"]
                    if duration >= 0.25:
                        speed_gain = group[end]["speed"] - group[start]["speed"]
                        if best is None or speed_gain > best[0]:
                            best = (speed_gain, start, end)
                start = None
        if best is None:
            continue
        _, begin, end = best
        duration = group[end]["time"] - group[begin]["time"]
        if duration <= 0.0:
            continue
        top_speed = max(row["speed"] for row in group[begin : end + 1])
        acceleration_g = ((group[end]["speed"] - group[begin]["speed"]) / 3.6) / duration / STANDARD_GRAVITY
        results.append(
            {
                "entry_speed_kmh": group[begin]["speed"],
                "top_speed_kmh": top_speed,
                "acceleration_g": acceleration_g,
            }
        )
    return results


def _straight_attribution(
    entry_delta: float | None,
    accel_delta: float | None,
    top_delta: float | None,
) -> str:
    if entry_delta is None or accel_delta is None or top_delta is None:
        return "data_limited"
    top = abs(top_delta)
    if top < 0.75:
        return "within_noise"
    if abs(entry_delta) >= top * 0.60 and abs(accel_delta) < 0.04:
        return "exit_or_line_driven" if entry_delta > 0.0 else "exit_or_line_loss"
    if abs(entry_delta) < 0.75 and abs(accel_delta) >= 0.04:
        return "forward_bite_or_power_delivery" if accel_delta > 0.0 else "reduced_forward_bite"
    if entry_delta > 0.75 and accel_delta > 0.04:
        return "exit_plus_acceleration"
    if entry_delta < -0.75 and accel_delta > 0.04:
        return "recovery_after_poor_exit"
    return "mixed_or_driving_line"


def _straight_analysis(
    previous: list[dict[str, Any]], current: list[dict[str, Any]]
) -> dict[str, Any]:
    before = _best_straights(previous)
    after = _best_straights(current)
    before_entry = _median(item["entry_speed_kmh"] for item in before)
    after_entry = _median(item["entry_speed_kmh"] for item in after)
    before_top = _median(item["top_speed_kmh"] for item in before)
    after_top = _median(item["top_speed_kmh"] for item in after)
    before_accel = _median(item["acceleration_g"] for item in before)
    after_accel = _median(item["acceleration_g"] for item in after)
    entry_delta = after_entry - before_entry if after_entry is not None and before_entry is not None else None
    top_delta = after_top - before_top if after_top is not None and before_top is not None else None
    accel_delta = after_accel - before_accel if after_accel is not None and before_accel is not None else None
    return {
        "status": "measured" if len(before) >= 3 and len(after) >= 3 else "data_limited",
        "straight_entry_speed_delta_kmh": _rounded(entry_delta),
        "straight_top_speed_delta_kmh": _rounded(top_delta),
        "straight_acceleration_delta_g": _rounded(accel_delta),
        "attribution": _straight_attribution(entry_delta, accel_delta, top_delta),
        "previous_straights": len(before),
        "current_straights": len(after),
        "evidence_id": "dynamics:straight:exit-vs-acceleration",
        "interpretation": (
            "Top speed is separated into speed already carried out of the corner and "
            "speed gained down the straight. Exit speed points to line/corner exit; "
            "acceleration points to forward bite or power delivery."
        ),
    }


def _corner_balance_analysis(
    previous: list[dict[str, Any]], current: list[dict[str, Any]]
) -> dict[str, Any]:
    def values(rows: list[dict[str, Any]]) -> dict[str, float | None]:
        lateral_per_steer: list[float] = []
        steering_for_lateral: list[float] = []
        yaw_per_steer: list[float] = []
        for row in _complete_rows(rows):
            steering = row["steering"]
            brake = row["brake"]
            if (
                steering is None
                or brake is None
                or row["speed"] < 8.0
                or abs(steering) < 8.0
                or brake > 5.0
            ):
                continue
            abs_steering = abs(steering)
            abs_lateral = abs(row["lateral"])
            lateral_per_steer.append(abs_lateral / max(8.0, abs_steering))
            if abs_lateral >= 0.45:
                steering_for_lateral.append(abs_steering / abs_lateral)
            if row["yaw"] is not None:
                yaw_per_steer.append(abs(row["yaw"]) / max(8.0, abs_steering))
        return {
            "lateral_per_steer": _median(lateral_per_steer),
            "steering_for_lateral": _median(steering_for_lateral),
            "yaw_per_steer": _median(yaw_per_steer),
        }

    before = values(previous)
    after = values(current)
    steering_delta = (
        after["steering_for_lateral"] - before["steering_for_lateral"]
        if after["steering_for_lateral"] is not None and before["steering_for_lateral"] is not None
        else None
    )
    yaw_delta = (
        after["yaw_per_steer"] - before["yaw_per_steer"]
        if after["yaw_per_steer"] is not None and before["yaw_per_steer"] is not None
        else None
    )
    status = "measured" if steering_delta is not None or yaw_delta is not None else "data_limited"
    if steering_delta is not None and steering_delta > 8.0:
        status = "more_steering_for_same_load"
    if yaw_delta is not None and yaw_delta > 0.08:
        status = "more_rotation_per_steering"
    return {
        "status": status,
        "steering_for_lateral_g_delta_percent": _rounded(steering_delta),
        "yaw_per_steering_delta_dps": _rounded(yaw_delta),
        "lateral_per_steering_delta_g": _rounded(
            after["lateral_per_steer"] - before["lateral_per_steer"]
            if after["lateral_per_steer"] is not None and before["lateral_per_steer"] is not None
            else None
        ),
        "evidence_id": "dynamics:corner:input-output-balance",
        "interpretation": (
            "This is the corner feel check. More yaw per steering supports rotation. "
            "More steering needed for the same lateral load supports push/fade, not more grip by itself."
        ),
    }


def _overdriving_values(rows: list[dict[str, Any]]) -> dict[str, Any]:
    samples: list[dict[str, float]] = []
    previous_by_lap: dict[int, dict[str, Any]] = {}
    for row in _derived_acceleration(rows):
        steering = row["steering"]
        brake = row["brake"]
        if (
            steering is None
            or brake is None
            or row["speed"] < 8.0
            or abs(steering) < 15.0
            or brake > 15.0
        ):
            previous_by_lap[row["lap"]] = row
            continue
        previous = previous_by_lap.get(row["lap"])
        correction = 0.0
        if previous is not None and previous.get("steering") is not None:
            correction = 1.0 if abs(steering - previous["steering"]) >= 15.0 else 0.0
        samples.append(
            {
                "steering": abs(steering),
                "lateral": abs(row["lateral"]),
                "non_brake_decel": max(0.0, -row["speed_accel_g"]) if brake <= 10.0 else 0.0,
                "correction": correction,
            }
        )
        previous_by_lap[row["lap"]] = row

    response_ratios = [
        sample["lateral"] / sample["steering"]
        for sample in samples
        if sample["steering"] >= 15.0 and sample["lateral"] >= 0.05
    ]
    efficient_response = _quantile(response_ratios, 0.75)
    if len(samples) < 20 or efficient_response is None:
        return {
            "index": None,
            "risk_sample_percent": None,
            "late_delta_score": None,
            "sample_count": len(samples),
        }

    scores: list[float] = []
    risk_samples = 0
    for sample in samples:
        expected_lateral = efficient_response * sample["steering"]
        weak_response = (
            max(0.0, min(1.0, (expected_lateral - sample["lateral"]) / expected_lateral))
            if expected_lateral > 0.05
            else 0.0
        )
        steering_stress = max(0.0, min(1.0, (sample["steering"] - 25.0) / 50.0))
        scrub_decel = max(0.0, min(1.0, sample["non_brake_decel"] / 0.25))
        score = 100.0 * steering_stress * (
            0.50 * weak_response + 0.35 * scrub_decel + 0.15 * sample["correction"]
        )
        scores.append(score)
        if score >= 20.0:
            risk_samples += 1

    late_delta = None
    if len(scores) >= 40:
        half = len(scores) // 2
        early = _quantile(scores[:half], 0.75)
        late = _quantile(scores[half:], 0.75)
        if early is not None and late is not None:
            late_delta = late - early

    return {
        "index": _quantile(scores, 0.75),
        "risk_sample_percent": risk_samples / max(1, len(scores)) * 100.0,
        "late_delta_score": late_delta,
        "sample_count": len(samples),
    }


def _overdriving_status(
    index_delta: float | None,
    risk_sample_delta: float | None,
    current_late_delta: float | None,
    lap_time_delta: float | None,
) -> str:
    if index_delta is None and risk_sample_delta is None and current_late_delta is None:
        return "data_limited"
    index = index_delta or 0.0
    risk = risk_sample_delta or 0.0
    late = current_late_delta or 0.0
    if index >= 8.0 or risk >= 6.0:
        if lap_time_delta is not None and lap_time_delta >= -0.05:
            return "more_overdriving_no_lap_gain"
        if late >= 8.0:
            return "more_overdriving_late_tire_risk"
        return "more_overdriving"
    if index <= -8.0 or risk <= -6.0:
        return "less_overdriving"
    if late >= 10.0:
        return "late_run_tire_scrub_risk"
    return "within_noise"


def _overdriving_analysis(
    previous: list[dict[str, Any]], current: list[dict[str, Any]], lap_time_delta: float | None
) -> dict[str, Any]:
    before = _overdriving_values(previous)
    after = _overdriving_values(current)
    index_delta = (
        after["index"] - before["index"]
        if after["index"] is not None and before["index"] is not None
        else None
    )
    risk_delta = (
        after["risk_sample_percent"] - before["risk_sample_percent"]
        if after["risk_sample_percent"] is not None
        and before["risk_sample_percent"] is not None
        else None
    )
    status = _overdriving_status(index_delta, risk_delta, after["late_delta_score"], lap_time_delta)
    return {
        "status": status,
        "overdriving_index_delta": _rounded(index_delta, 2),
        "overdriving_risk_sample_delta_percent": _rounded(risk_delta, 2),
        "current_late_overdriving_delta_score": _rounded(after["late_delta_score"], 2),
        "previous": {
            "overdriving_index": _rounded(before["index"], 2),
            "risk_sample_percent": _rounded(before["risk_sample_percent"], 2),
            "late_delta_score": _rounded(before["late_delta_score"], 2),
            "sample_count": before["sample_count"],
        },
        "current": {
            "overdriving_index": _rounded(after["index"], 2),
            "risk_sample_percent": _rounded(after["risk_sample_percent"], 2),
            "late_delta_score": _rounded(after["late_delta_score"], 2),
            "sample_count": after["sample_count"],
        },
        "thresholds": {
            "more_overdriving_index_delta": 8.0,
            "more_overdriving_risk_sample_delta_percent": 6.0,
            "late_run_tire_scrub_delta_score": 10.0,
            "risk_sample_score": 20.0,
        },
        "evidence_id": "dynamics:driver:overdriving-tire-scrub",
        "interpretation": (
            "This flags extra steering/correction and no-brake speed bleed when the car does not "
            "give matching cornering response. It means possible tire scrub or pushing past the "
            "working range, not driver blame and not measured tire temperature."
        ),
    }


def _roll_angle_deg(lateral_g: float | None, vertical_g: float | None) -> float | None:
    if lateral_g is None or vertical_g is None or abs(vertical_g) < 0.15:
        return None
    return math.degrees(math.atan2(lateral_g, abs(vertical_g)))


def _chassis_roll_values(rows: list[dict[str, Any]]) -> dict[str, Any]:
    stopped: list[float] = []
    straight: list[float] = []
    for row in rows:
        lateral = row.get("raw_lateral")
        vertical = row.get("raw_vertical")
        roll = _roll_angle_deg(lateral, vertical)
        if roll is None:
            continue
        steering = row["steering"]
        brake = row["brake"]
        if row["speed"] <= 1.5:
            stopped.append(roll)
        elif (
            row["speed"] >= 8.0
            and abs(lateral or 0.0) <= 0.20
            and (
                steering is None
                or brake is None
                or (abs(steering) <= 5.0 and brake <= 5.0)
            )
        ):
            straight.append(roll)

    if len(stopped) >= 25:
        surface = _median(stopped)
        source = "stopped_points"
        samples = len(stopped)
    elif len(straight) >= 25:
        surface = _median(straight)
        source = "straight"
        samples = len(straight)
    else:
        surface = None
        source = "data_limited"
        samples = max(len(stopped), len(straight))

    roll_signature: list[float] = []
    roll_per_lateral: list[float] = []
    if surface is not None:
        for row in _complete_rows(rows):
            steering = row["steering"]
            brake = row["brake"]
            raw_lateral = row.get("raw_lateral")
            raw_vertical = row.get("raw_vertical")
            roll = _roll_angle_deg(raw_lateral, raw_vertical)
            if (
                steering is None
                or brake is None
                or roll is None
                or row["speed"] < 8.0
                or abs(steering) < 15.0
                or brake > 15.0
                or abs(row["lateral"]) < 0.35
            ):
                continue
            corrected = abs(roll - surface)
            roll_signature.append(corrected)
            roll_per_lateral.append(corrected / max(0.35, abs(row["lateral"])))

    return {
        "surface_tilt_roll_deg": surface,
        "surface_tilt_source": source,
        "surface_tilt_samples": samples,
        "chassis_roll_signature_deg": _median(roll_signature),
        "roll_per_lateral_g_deg": _median(roll_per_lateral),
    }


def _chassis_roll_status(
    roll_delta: float | None,
    roll_per_lateral_delta: float | None,
    surface_tilt_delta: float | None,
) -> str:
    if roll_delta is None and roll_per_lateral_delta is None:
        return "data_limited"
    if surface_tilt_delta is not None and abs(surface_tilt_delta) >= 2.0:
        return "surface_tilt_changed_check_reference"
    roll = roll_delta or 0.0
    normalized = roll_per_lateral_delta or 0.0
    if roll >= 0.75 or normalized >= 0.75:
        return "more_chassis_roll_signature"
    if roll <= -0.75 or normalized <= -0.75:
        return "less_chassis_roll_signature"
    return "within_noise"


def _chassis_roll_analysis(
    previous: list[dict[str, Any]], current: list[dict[str, Any]]
) -> dict[str, Any]:
    before = _chassis_roll_values(previous)
    after = _chassis_roll_values(current)
    surface_delta = (
        after["surface_tilt_roll_deg"] - before["surface_tilt_roll_deg"]
        if after["surface_tilt_roll_deg"] is not None
        and before["surface_tilt_roll_deg"] is not None
        else None
    )
    roll_delta = (
        after["chassis_roll_signature_deg"] - before["chassis_roll_signature_deg"]
        if after["chassis_roll_signature_deg"] is not None
        and before["chassis_roll_signature_deg"] is not None
        else None
    )
    per_lateral_delta = (
        after["roll_per_lateral_g_deg"] - before["roll_per_lateral_g_deg"]
        if after["roll_per_lateral_g_deg"] is not None
        and before["roll_per_lateral_g_deg"] is not None
        else None
    )
    return {
        "status": _chassis_roll_status(roll_delta, per_lateral_delta, surface_delta),
        "surface_tilt_delta_deg": _rounded(surface_delta, 2),
        "chassis_roll_delta_deg": _rounded(roll_delta, 2),
        "roll_per_lateral_g_delta_deg": _rounded(per_lateral_delta, 2),
        "current_surface_tilt_source": after["surface_tilt_source"],
        "current_surface_tilt_samples": after["surface_tilt_samples"],
        "previous": {
            "surface_tilt_roll_deg": _rounded(before["surface_tilt_roll_deg"], 2),
            "surface_tilt_source": before["surface_tilt_source"],
            "surface_tilt_samples": before["surface_tilt_samples"],
            "chassis_roll_signature_deg": _rounded(before["chassis_roll_signature_deg"], 2),
            "roll_per_lateral_g_deg": _rounded(before["roll_per_lateral_g_deg"], 2),
        },
        "current": {
            "surface_tilt_roll_deg": _rounded(after["surface_tilt_roll_deg"], 2),
            "surface_tilt_source": after["surface_tilt_source"],
            "surface_tilt_samples": after["surface_tilt_samples"],
            "chassis_roll_signature_deg": _rounded(after["chassis_roll_signature_deg"], 2),
            "roll_per_lateral_g_deg": _rounded(after["roll_per_lateral_g_deg"], 2),
        },
        "thresholds": {
            "roll_signature_delta_deg": 0.75,
            "surface_tilt_reference_warning_deg": 2.0,
            "minimum_stopped_or_straight_samples": 25,
        },
        "evidence_id": "dynamics:chassis:tilt-corrected-roll-signature",
        "interpretation": (
            "Stopped points are used first, then straights, to estimate asphalt/sensor tilt. "
            "The corrected corner value is a chassis roll signature, not shock travel or exact tire load."
        ),
    }


def _roll_rate_values(rows: list[dict[str, Any]]) -> dict[str, Any]:
    roll_reference = _chassis_roll_values(rows)
    surface = roll_reference["surface_tilt_roll_deg"]
    if surface is None:
        return {
            **roll_reference,
            "roll_rate_p90_dps": None,
            "roll_rate_per_lateral_g_dps": None,
            "late_roll_rate_delta_dps": None,
            "roll_rate_samples": 0,
        }

    corrected: list[float | None] = []
    for row in rows:
        roll = _roll_angle_deg(row.get("raw_lateral"), row.get("raw_vertical"))
        corrected.append(roll - surface if roll is not None else None)

    smoothed: list[float | None] = [None] * len(rows)
    for index, row in enumerate(rows):
        total = 0.0
        count = 0
        for sample_index in range(max(0, index - 2), min(len(rows), index + 3)):
            value = corrected[sample_index]
            if value is None or rows[sample_index]["lap"] != row["lap"]:
                continue
            dt = abs(rows[sample_index]["time"] - row["time"])
            if dt > 0.100:
                continue
            total += value
            count += 1
        if count >= 2:
            smoothed[index] = total / count

    rates: list[float] = []
    rates_per_lateral: list[float] = []
    for index, row in enumerate(rows):
        if index < 2 or index + 2 >= len(rows):
            continue
        steering = row["steering"]
        brake = row["brake"]
        if (
            row["phase"] != "complete"
            or steering is None
            or brake is None
            or row["speed"] < 8.0
            or abs(steering) < 15.0
            or brake > 15.0
            or abs(row["lateral"]) < 0.35
            or rows[index - 2]["lap"] != row["lap"]
            or rows[index + 2]["lap"] != row["lap"]
            or smoothed[index - 2] is None
            or smoothed[index + 2] is None
        ):
            continue
        dt = rows[index + 2]["time"] - rows[index - 2]["time"]
        if dt < 0.080 or dt > 0.240:
            continue
        rate = (smoothed[index + 2] - smoothed[index - 2]) / dt
        if not math.isfinite(rate):
            continue
        absolute = abs(rate)
        rates.append(absolute)
        rates_per_lateral.append(absolute / max(0.35, abs(row["lateral"])))

    late_delta = None
    if len(rates) >= 40:
        half = len(rates) // 2
        early = _quantile(rates[:half], 0.90)
        late = _quantile(rates[half:], 0.90)
        if early is not None and late is not None:
            late_delta = late - early

    return {
        **roll_reference,
        "roll_rate_p90_dps": _quantile(rates, 0.90),
        "roll_rate_per_lateral_g_dps": _quantile(rates_per_lateral, 0.90),
        "late_roll_rate_delta_dps": late_delta,
        "roll_rate_samples": len(rates),
    }


def _roll_rate_status(
    before: dict[str, Any],
    after: dict[str, Any],
    surface_tilt_delta: float | None,
) -> str:
    if (
        before["roll_rate_p90_dps"] is None
        or after["roll_rate_p90_dps"] is None
        or before["roll_rate_samples"] < 25
        or after["roll_rate_samples"] < 25
    ):
        return "data_limited"
    if surface_tilt_delta is not None and abs(surface_tilt_delta) >= 2.0:
        return "surface_tilt_changed_check_reference"
    delta = after["roll_rate_p90_dps"] - before["roll_rate_p90_dps"]
    threshold = max(8.0, abs(before["roll_rate_p90_dps"]) * 0.15)
    before_normalized = before.get("roll_rate_per_lateral_g_dps")
    after_normalized = after.get("roll_rate_per_lateral_g_dps")
    normalized_delta = (
        after_normalized - before_normalized
        if before_normalized is not None and after_normalized is not None
        else None
    )
    normalized_threshold = max(8.0, abs(before_normalized or 0.0) * 0.15)
    if delta >= threshold or (
        normalized_delta is not None and normalized_delta >= normalized_threshold
    ):
        return "faster_roll_build"
    if delta <= -threshold or (
        normalized_delta is not None and normalized_delta <= -normalized_threshold
    ):
        return "slower_roll_build"
    return "within_noise"


def _roll_rate_analysis(
    previous: list[dict[str, Any]], current: list[dict[str, Any]]
) -> dict[str, Any]:
    before = _roll_rate_values(previous)
    after = _roll_rate_values(current)
    surface_delta = (
        after["surface_tilt_roll_deg"] - before["surface_tilt_roll_deg"]
        if after["surface_tilt_roll_deg"] is not None
        and before["surface_tilt_roll_deg"] is not None
        else None
    )
    rate_delta = (
        after["roll_rate_p90_dps"] - before["roll_rate_p90_dps"]
        if after["roll_rate_p90_dps"] is not None
        and before["roll_rate_p90_dps"] is not None
        else None
    )
    normalized_delta = (
        after["roll_rate_per_lateral_g_dps"] - before["roll_rate_per_lateral_g_dps"]
        if after["roll_rate_per_lateral_g_dps"] is not None
        and before["roll_rate_per_lateral_g_dps"] is not None
        else None
    )
    return {
        "status": _roll_rate_status(before, after, surface_delta),
        "previous_roll_rate_p90_dps": _rounded(before["roll_rate_p90_dps"], 2),
        "current_roll_rate_p90_dps": _rounded(after["roll_rate_p90_dps"], 2),
        "roll_rate_delta_dps": _rounded(rate_delta, 2),
        "roll_rate_per_lateral_g_delta_dps": _rounded(normalized_delta, 2),
        "current_late_roll_rate_delta_dps": _rounded(after["late_roll_rate_delta_dps"], 2),
        "previous_roll_rate_samples": before["roll_rate_samples"],
        "current_roll_rate_samples": after["roll_rate_samples"],
        "thresholds": {
            "minimum_delta_dps": 8.0,
            "adaptive_baseline_fraction": 0.15,
            "minimum_cornering_samples": 25,
            "smoothing_window_ms": 200,
            "derivative_window_ms": 160,
            "software_range": "uncapped_deg_per_second",
        },
        "evidence_id": "dynamics:chassis:tilt-corrected-roll-rate",
        "interpretation": (
            "This measures how quickly the tilt-corrected chassis loading signature builds. "
            "It is useful for take-a-set response. It is not exact suspension travel, and if a "
            "physical sensor clips, the true peak above that range cannot be recovered."
        ),
    }


def _braking_analysis(rows: list[dict[str, Any]]) -> dict[str, Any]:
    derived = _derived_acceleration(rows)
    events: list[list[dict[str, Any]]] = []
    active: list[dict[str, Any]] = []
    for row in derived:
        eligible = (
            row["brake"] is not None
            and row["throttle"] is not None
            and row["brake"] >= 70.0
            and row["throttle"] <= 5.0
            and row["speed"] >= 10.0
        )
        contiguous = (
            active
            and row["lap"] == active[-1]["lap"]
            and 0.0 < row["time"] - active[-1]["time"] <= 0.250
        )
        if eligible:
            if active and not contiguous:
                events.append(active)
                active = []
            active.append(row)
        elif active:
            events.append(active)
            active = []
    if active:
        events.append(active)

    sustained = 0
    possible = 0
    disturbed = 0
    peak_decelerations: list[float] = []
    response_delays: list[float] = []
    for event in events:
        if event[-1]["time"] - event[0]["time"] < 0.080:
            continue
        sustained += 1
        deceleration = [-row["speed_accel_g"] for row in event]
        peak = max(deceleration)
        peak_decelerations.append(peak)
        delay = None
        flagged = False
        for index, value in enumerate(deceleration):
            if value >= 0.20:
                delay = event[index]["time"] - event[0]["time"]
                response_delays.append(delay)
                break
        if delay is None or delay >= 0.080 or peak < 0.20:
            possible += 1
            flagged = True
        peak_index = deceleration.index(peak)
        low_start: int | None = None
        for index in range(peak_index + 1, len(event)):
            if deceleration[index] <= peak - 0.20:
                if low_start is None:
                    low_start = index
                if event[index]["time"] - event[low_start]["time"] >= 0.080:
                    if not flagged:
                        possible += 1
                        flagged = True
                    steering_values = [
                        row["steering"] for row in event if row["steering"] is not None
                    ]
                    steering_change = (
                        max(steering_values) - min(steering_values)
                        if steering_values else 0.0
                    )
                    yaw_peak = max(
                        (abs(row["yaw"]) for row in event if row["yaw"] is not None),
                        default=0.0,
                    )
                    lateral_peak = max(abs(row["lateral"]) for row in event)
                    if yaw_peak >= 45.0 or lateral_peak >= 0.45 or steering_change >= 20.0:
                        disturbed += 1
                    break
            else:
                low_start = None
    return {
        "sustained_high_brake_events": sustained,
        "possible_lockup_or_low_grip_indicators": possible,
        "indicators_with_yaw_lateral_or_steering_disturbance": disturbed,
        "p90_peak_speed_derived_deceleration_g": _rounded(_quantile(peak_decelerations, 0.90)),
        "median_brake_response_delay_s": _rounded(_median(response_delays)),
        "thresholds": {
            "brake_percent": 70.0,
            "minimum_duration_ms": 80,
            "minimum_deceleration_response_g": 0.20,
            "maximum_response_delay_ms": 80,
            "deceleration_drop_g": 0.20,
            "disturbance_yaw_dps": 45.0,
            "disturbance_lateral_g": 0.45,
            "disturbance_steering_change_percent": 20.0,
        },
        "evidence_id": "dynamics:braking:possible-lockup-low-grip",
        "interpretation": (
            "This is an anomaly indicator only. Chassis GNSS/IMU and brake command cannot confirm "
            "that a tire locked; wheel-speed data is required."
        ),
    }


def _quality_confidence(
    previous_laps: dict[str, Any],
    current_laps: dict[str, Any],
    previous_quality: dict[str, Any],
    current_quality: dict[str, Any],
) -> int:
    score = 20
    score += min(25, min(previous_laps["complete_laps"], current_laps["complete_laps"]) * 8)
    score += int(20 * min(previous_quality["radio_coverage"], current_quality["radio_coverage"]))
    periods = [
        previous_quality["median_sample_period_ms"],
        current_quality["median_sample_period_ms"],
    ]
    if all(period is not None and period <= 50.0 for period in periods):
        score += 15
    elif all(period is not None and period <= 100.0 for period in periods):
        score += 8
    satellites = [
        previous_quality["median_satellites"],
        current_quality["median_satellites"],
    ]
    if all(value is not None and value >= 10 for value in satellites):
        score += 10
    return max(0, min(100, score))


def _track_shape(rows: list[dict[str, Any]]) -> dict[int, tuple[float, float]]:
    bins: dict[int, list[tuple[float, float]]] = defaultdict(list)
    for row in _complete_rows(rows):
        if row["latitude"] is None or row["longitude"] is None:
            continue
        progress_bin = int(max(0.0, min(99.999, row["progress"])) // 5.0)
        bins[progress_bin].append((row["latitude"], row["longitude"]))
    return {
        key: (
            statistics.median(point[0] for point in points),
            statistics.median(point[1] for point in points),
        )
        for key, points in bins.items()
        if len(points) >= 3
    }


def _track_compatibility(
    previous: list[dict[str, Any]], current: list[dict[str, Any]]
) -> dict[str, Any]:
    before = _track_shape(previous)
    after = _track_shape(current)
    shared = sorted(before.keys() & after.keys())
    if len(shared) < 10:
        return {
            "status": "data_limited",
            "shared_progress_bins": len(shared),
            "translation_m": None,
            "residual_rms_m": None,
            "evidence_id": "quality:track-shape",
        }
    reference_latitude = statistics.median(
        [before[key][0] for key in shared] + [after[key][0] for key in shared]
    )
    metres_per_degree_lat = 111_320.0
    metres_per_degree_lon = metres_per_degree_lat * math.cos(
        math.radians(reference_latitude)
    )
    offsets: list[tuple[float, float]] = []
    for key in shared:
        before_lat, before_lon = before[key]
        after_lat, after_lon = after[key]
        offsets.append(
            (
                (after_lon - before_lon) * metres_per_degree_lon,
                (after_lat - before_lat) * metres_per_degree_lat,
            )
        )
    east = statistics.median(value[0] for value in offsets)
    north = statistics.median(value[1] for value in offsets)
    residual = math.sqrt(
        sum((x - east) ** 2 + (y - north) ** 2 for x, y in offsets)
        / len(offsets)
    )
    return {
        "status": "compatible" if residual <= 5.0 else "incompatible",
        "shared_progress_bins": len(shared),
        "translation_m": _rounded(math.hypot(east, north)),
        "residual_rms_m": _rounded(residual),
        "maximum_residual_rms_m": 5.0,
        "evidence_id": "quality:track-shape",
        "interpretation": (
            "One whole-run east/north translation is removed only for track-shape validation. "
            "The displayed and stored raw GPS is not changed."
        ),
    }


def _progress_bins_between(start: int, end: int) -> list[int]:
    """Return inclusive one-percent bins while moving forward around the lap."""

    start %= SESSION_PROGRESS_BINS
    end %= SESSION_PROGRESS_BINS
    if start <= end:
        return list(range(start, end + 1))
    return list(range(start, SESSION_PROGRESS_BINS)) + list(range(0, end + 1))


def _session_progress_profiles(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """Build aligned per-lap one-percent profiles without returning raw coordinates."""

    lap_bins: dict[int, dict[int, list[dict[str, Any]]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for row in _complete_rows(rows):
        if row["latitude"] is None or row["longitude"] is None:
            continue
        progress_bin = int(
            max(0.0, min(99.999, row["progress"]))
            // (100.0 / SESSION_PROGRESS_BINS)
        )
        lap_bins[row["lap"]][progress_bin].append(row)

    raw_profiles: dict[int, dict[int, dict[str, float]]] = {}
    for lap, bins in lap_bins.items():
        profile: dict[int, dict[str, float]] = {}
        for progress_bin, samples in bins.items():
            latitude = _median(
                row["latitude"] for row in samples if row["latitude"] is not None
            )
            longitude = _median(
                row["longitude"] for row in samples if row["longitude"] is not None
            )
            if latitude is None or longitude is None:
                continue
            profile[progress_bin] = {
                "latitude": latitude,
                "longitude": longitude,
                "speed": _median(row["speed"] for row in samples) or 0.0,
                "lateral": _median(row["lateral"] for row in samples) or 0.0,
                "longitudinal": _median(row["longitudinal"] for row in samples)
                or 0.0,
                "yaw": _median(
                    row["yaw"] for row in samples if row["yaw"] is not None
                ),
                "lap_elapsed": _median(row["lap_elapsed"] for row in samples)
                or 0.0,
            }
        if len(profile) >= 40:
            raw_profiles[lap] = profile

    if len(raw_profiles) < 2:
        return {
            "status": "data_limited",
            "reason": "At least two complete laps with GPS coverage are required.",
            "profiles": {},
            "centerline": {},
        }

    shared_coordinates = [
        (point["latitude"], point["longitude"])
        for profile in raw_profiles.values()
        for point in profile.values()
    ]
    reference_latitude = _median(point[0] for point in shared_coordinates)
    reference_longitude = _median(point[1] for point in shared_coordinates)
    if reference_latitude is None or reference_longitude is None:
        return {
            "status": "data_limited",
            "reason": "The GPS coordinates were incomplete.",
            "profiles": {},
            "centerline": {},
        }
    metres_per_degree_lat = 111_320.0
    metres_per_degree_lon = metres_per_degree_lat * math.cos(
        math.radians(reference_latitude)
    )

    provisional: dict[int, tuple[float, float]] = {}
    for progress_bin in range(SESSION_PROGRESS_BINS):
        points = [
            (
                (profile[progress_bin]["longitude"] - reference_longitude)
                * metres_per_degree_lon,
                (profile[progress_bin]["latitude"] - reference_latitude)
                * metres_per_degree_lat,
            )
            for profile in raw_profiles.values()
            if progress_bin in profile
        ]
        if len(points) >= 2:
            provisional[progress_bin] = (
                statistics.median(point[0] for point in points),
                statistics.median(point[1] for point in points),
            )

    aligned_profiles: dict[int, dict[int, dict[str, float]]] = {}
    translations: dict[int, float] = {}
    for lap, profile in raw_profiles.items():
        offsets = []
        for progress_bin, point in profile.items():
            if progress_bin not in provisional:
                continue
            x = (point["longitude"] - reference_longitude) * metres_per_degree_lon
            y = (point["latitude"] - reference_latitude) * metres_per_degree_lat
            centre_x, centre_y = provisional[progress_bin]
            offsets.append((x - centre_x, y - centre_y))
        if len(offsets) < 30:
            continue
        east = statistics.median(value[0] for value in offsets)
        north = statistics.median(value[1] for value in offsets)
        translations[lap] = math.hypot(east, north)
        aligned: dict[int, dict[str, float]] = {}
        for progress_bin, point in profile.items():
            aligned[progress_bin] = {
                **point,
                "x": (
                    (point["longitude"] - reference_longitude)
                    * metres_per_degree_lon
                    - east
                ),
                "y": (
                    (point["latitude"] - reference_latitude)
                    * metres_per_degree_lat
                    - north
                ),
            }
        aligned_profiles[lap] = aligned

    centerline: dict[int, dict[str, float]] = {}
    for progress_bin in range(SESSION_PROGRESS_BINS):
        points = [
            profile[progress_bin]
            for profile in aligned_profiles.values()
            if progress_bin in profile
        ]
        if len(points) < 2:
            continue
        centerline[progress_bin] = {
            "x": statistics.median(point["x"] for point in points),
            "y": statistics.median(point["y"] for point in points),
            "speed": statistics.median(point["speed"] for point in points),
            "lateral": statistics.median(point["lateral"] for point in points),
            "longitudinal": statistics.median(
                point["longitudinal"] for point in points
            ),
            "yaw": _median(
                point["yaw"] for point in points if point.get("yaw") is not None
            ),
        }

    residuals: dict[int, float] = {}
    for lap, profile in aligned_profiles.items():
        squared = [
            (point["x"] - centerline[progress_bin]["x"]) ** 2
            + (point["y"] - centerline[progress_bin]["y"]) ** 2
            for progress_bin, point in profile.items()
            if progress_bin in centerline
        ]
        if squared:
            residuals[lap] = math.sqrt(sum(squared) / len(squared))

    edge_lengths: dict[int, float] = {}
    for progress_bin in range(SESSION_PROGRESS_BINS):
        following = (progress_bin + 1) % SESSION_PROGRESS_BINS
        if progress_bin in centerline and following in centerline:
            edge_lengths[progress_bin] = math.hypot(
                centerline[following]["x"] - centerline[progress_bin]["x"],
                centerline[following]["y"] - centerline[progress_bin]["y"],
            )
    typical_edge = _median(edge_lengths.values()) or 0.0
    for progress_bin in range(SESSION_PROGRESS_BINS):
        edge_lengths.setdefault(progress_bin, typical_edge)

    return {
        "status": "measured" if len(centerline) >= 60 else "data_limited",
        "reason": (
            ""
            if len(centerline) >= 60
            else "Fewer than 60 one-percent track bins had repeatable GPS coverage."
        ),
        "profiles": aligned_profiles,
        "centerline": centerline,
        "translations": translations,
        "residuals": residuals,
        "edge_lengths": edge_lengths,
        "track_length_m": sum(edge_lengths.values()),
    }


def _detected_corner_regions(centerline: dict[int, dict[str, float]]) -> list[dict[str, Any]]:
    if len(centerline) < 60:
        return []
    scores = {
        progress_bin: abs(point["lateral"])
        for progress_bin, point in centerline.items()
    }
    available = list(scores.values())
    maximum = max(available, default=0.0)
    threshold = max(0.16, (_quantile(available, 0.60) or 0.0), maximum * 0.32)
    active = sorted(
        progress_bin
        for progress_bin, score in scores.items()
        if score >= threshold
    )
    if not active:
        return []

    groups: list[list[int]] = []
    group: list[int] = []
    for progress_bin in active:
        if group and progress_bin - group[-1] > 2:
            groups.append(group)
            group = []
        group.append(progress_bin)
    if group:
        groups.append(group)
    if (
        len(groups) >= 2
        and groups[0][0] <= 1
        and groups[-1][-1] >= SESSION_PROGRESS_BINS - 2
    ):
        groups[0] = groups[-1] + groups[0]
        groups.pop()

    corners: list[dict[str, Any]] = []
    for bins in groups:
        unique_bins = sorted(set(value % SESSION_PROGRESS_BINS for value in bins))
        if len(unique_bins) < 2 or len(unique_bins) > 30:
            continue
        apex = max(unique_bins, key=lambda value: scores.get(value, 0.0))
        ordered = sorted(
            unique_bins,
            key=lambda value: (value - apex) % SESSION_PROGRESS_BINS,
        )
        # Recover the actual travel-order boundaries around the apex.
        start = min(unique_bins)
        end = max(unique_bins)
        wraps = unique_bins[0] <= 1 and unique_bins[-1] >= 98
        if wraps:
            high = [value for value in unique_bins if value >= 50]
            low = [value for value in unique_bins if value < 50]
            start = min(high) if high else start
            end = max(low) if low else end
        direction_value = _median(
            centerline[value]["lateral"]
            for value in unique_bins
            if value in centerline
        ) or 0.0
        corners.append(
            {
                "active_start_bin": start,
                "active_end_bin": end,
                "start_bin": (start - 2) % SESSION_PROGRESS_BINS,
                "apex_bin": apex,
                "end_bin": (end + 2) % SESSION_PROGRESS_BINS,
                "approach_start_bin": (start - 10) % SESSION_PROGRESS_BINS,
                "wraps_start_finish": wraps,
                "direction": "left" if direction_value > 0.0 else "right",
                "peak_abs_lateral_g": scores.get(apex),
                "detection_threshold_abs_lateral_g": threshold,
            }
        )
    corners.sort(key=lambda item: item["apex_bin"])
    return corners[:20]


def _profile_value_near(
    profile: dict[int, dict[str, float]], progress_bin: int, field: str
) -> float | None:
    for distance in (0, 1, -1, 2, -2):
        point = profile.get((progress_bin + distance) % SESSION_PROGRESS_BINS)
        if point is not None and point.get(field) is not None:
            return point[field]
    return None


def _elapsed_between_bins(
    profile: dict[int, dict[str, float]],
    start_bin: int,
    end_bin: int,
    lap_duration: float,
) -> float | None:
    start = _profile_value_near(profile, start_bin, "lap_elapsed")
    end = _profile_value_near(profile, end_bin, "lap_elapsed")
    if start is None or end is None:
        return None
    difference = end - start
    if end_bin < start_bin or difference < 0.0:
        difference += lap_duration
    return difference if 0.0 < difference < lap_duration else None


def _corner_speed_summary(
    profile: dict[int, dict[str, float]], start_bin: int, end_bin: int
) -> dict[str, float | None]:
    speeds = [
        profile[value]["speed"]
        for value in _progress_bins_between(start_bin, end_bin)
        if value in profile
    ]
    return {
        "entry": _profile_value_near(profile, start_bin, "speed"),
        "minimum": min(speeds, default=None),
        "exit": _profile_value_near(profile, end_bin, "speed"),
        "average": None,
    }


def _distance_between_bins(
    edge_lengths: dict[int, float], start_bin: int, end_bin: int
) -> float:
    bins = _progress_bins_between(start_bin, end_bin)
    return sum(edge_lengths.get(progress_bin, 0.0) for progress_bin in bins[:-1])


def _speed_derived_slowing_onset(
    profile: dict[int, dict[str, float]],
    approach_start_bin: int,
    apex_bin: int,
    lap_duration: float,
    edge_lengths: dict[int, float],
) -> dict[str, Any] | None:
    ordered_bins = _progress_bins_between(approach_start_bin, apex_bin)
    candidates = [
        progress_bin for progress_bin in ordered_bins if progress_bin in profile
    ]
    if len(candidates) < 5:
        return None
    peak_deceleration = 0.0
    acceleration_samples: list[tuple[int, float]] = []
    first: dict[str, Any] | None = None
    for index in range(len(candidates) - 2):
        current_bin = candidates[index]
        future_bin = candidates[index + 2]
        current = profile[current_bin]
        future = profile[future_bin]
        dt = future["lap_elapsed"] - current["lap_elapsed"]
        if dt <= 0.0:
            dt += lap_duration
        if dt <= 0.0 or dt > 2.0:
            continue
        speed_loss = current["speed"] - future["speed"]
        acceleration_g = (
            ((future["speed"] - current["speed"]) / 3.6)
            / dt
            / STANDARD_GRAVITY
        )
        acceleration_samples.append((current_bin, acceleration_g))
        peak_deceleration = min(peak_deceleration, acceleration_g)
        required_loss = max(1.0, current["speed"] * 0.03)
        if first is None and speed_loss >= required_loss and acceleration_g <= -0.05:
            apex_elapsed = _profile_value_near(profile, apex_bin, "lap_elapsed")
            if apex_elapsed is None:
                continue
            time_before_apex = apex_elapsed - current["lap_elapsed"]
            if time_before_apex < 0.0:
                time_before_apex += lap_duration
            first = {
                "progress_bin": current_bin,
                "progress_percent": current_bin + 0.5,
                "speed_kmh": current["speed"],
                "distance_before_apex_m": _distance_between_bins(
                    edge_lengths, current_bin, apex_bin
                ),
                "time_before_apex_s": time_before_apex,
                "initial_speed_derived_deceleration_g": acceleration_g,
            }
    if first is not None:
        first_bin = int(first["progress_bin"])
        apex = profile.get(apex_bin)
        start = profile.get(first_bin)
        if apex is not None and start is not None:
            duration = float(first["time_before_apex_s"])
            mean_deceleration = (
                ((apex["speed"] - start["speed"]) / 3.6)
                / duration
                / STANDARD_GRAVITY
                if duration > 0.0
                else None
            )
            slowing_bins = [
                value
                for value in _progress_bins_between(first_bin, apex_bin)
                if value in profile
            ]
            negative_acceleration = [
                value
                for progress_bin, value in acceleration_samples
                if progress_bin in slowing_bins and value < 0.0
            ]
            longitudinal = [profile[value]["longitudinal"] for value in slowing_bins]
            lateral = [abs(profile[value]["lateral"]) for value in slowing_bins]
            yaw = [
                abs(profile[value]["yaw"])
                for value in slowing_bins
                if profile[value].get("yaw") is not None
            ]
            magnitude = abs(mean_deceleration or 0.0)
            first.update(
                {
                    "apex_speed_kmh": apex["speed"],
                    "speed_loss_to_apex_kmh": start["speed"] - apex["speed"],
                    "deceleration_duration_s_to_apex": duration,
                    "mean_speed_derived_deceleration_g_to_apex": mean_deceleration,
                    "median_negative_speed_derived_deceleration_g_to_apex": _median(
                        negative_acceleration
                    ),
                    "median_chassis_longitudinal_g_to_apex": _median(longitudinal),
                    "median_abs_chassis_lateral_g_to_apex": _median(lateral),
                    "median_abs_yaw_rate_dps_to_apex": _median(yaw),
                    "deceleration_character": (
                        "mild"
                        if magnitude < 0.12
                        else "moderate"
                        if magnitude < 0.30
                        else "strong"
                    ),
                }
            )
        first["peak_speed_derived_deceleration_g_to_apex"] = peak_deceleration
    return first


def _line_offset_at_bin(
    profile: dict[int, dict[str, float]],
    centerline: dict[int, dict[str, float]],
    progress_bin: int,
) -> float | None:
    before = centerline.get((progress_bin - 1) % SESSION_PROGRESS_BINS)
    after = centerline.get((progress_bin + 1) % SESSION_PROGRESS_BINS)
    point = profile.get(progress_bin)
    centre = centerline.get(progress_bin)
    if not all((before, after, point, centre)):
        return None
    tangent_x = after["x"] - before["x"]
    tangent_y = after["y"] - before["y"]
    length = math.hypot(tangent_x, tangent_y)
    if length <= 0.01:
        return None
    normal_x = -tangent_y / length
    normal_y = tangent_x / length
    return (
        (point["x"] - centre["x"]) * normal_x
        + (point["y"] - centre["y"]) * normal_y
    )


def _corner_line_offset(
    profile: dict[int, dict[str, float]],
    centerline: dict[int, dict[str, float]],
    apex_bin: int,
) -> float | None:
    return _line_offset_at_bin(profile, centerline, apex_bin)


def _corner_comparison_trace(
    best_profile: dict[int, dict[str, float]],
    comparison_profiles: list[dict[int, dict[str, float]]],
    centerline: dict[int, dict[str, float]],
    edge_lengths: dict[int, float],
    approach_start_bin: int,
    apex_bin: int,
    end_bin: int,
    best_duration: float,
    comparison_durations: list[float],
    line_available: bool,
) -> list[dict[str, Any]]:
    ordered = _progress_bins_between(approach_start_bin, end_bin)
    before_apex = set(_progress_bins_between(approach_start_bin, apex_bin))
    result: list[dict[str, Any]] = []
    for progress_bin in ordered:
        best = best_profile.get(progress_bin)
        comparison = [profile[progress_bin] for profile in comparison_profiles if progress_bin in profile]
        if best is None or not comparison:
            continue
        if progress_bin == approach_start_bin:
            best_elapsed = 0.0
        else:
            best_elapsed = _elapsed_between_bins(
                best_profile, approach_start_bin, progress_bin, best_duration
            )
        comparison_elapsed_values: list[float] = []
        for profile, duration in zip(comparison_profiles, comparison_durations):
            if progress_bin not in profile:
                continue
            elapsed = (
                0.0
                if progress_bin == approach_start_bin
                else _elapsed_between_bins(
                    profile, approach_start_bin, progress_bin, duration
                )
            )
            if elapsed is not None:
                comparison_elapsed_values.append(elapsed)
        typical_elapsed = _median(comparison_elapsed_values)
        best_line = (
            _line_offset_at_bin(best_profile, centerline, progress_bin)
            if line_available
            else None
        )
        typical_line = (
            _median(
                value
                for profile in comparison_profiles
                if (value := _line_offset_at_bin(profile, centerline, progress_bin))
                is not None
            )
            if line_available
            else None
        )
        line_delta = (
            best_line - typical_line
            if best_line is not None and typical_line is not None
            else None
        )
        if line_delta is not None and abs(line_delta) > 3.0:
            line_delta = None
        distance_from_apex = (
            0.0
            if progress_bin == apex_bin
            else -_distance_between_bins(edge_lengths, progress_bin, apex_bin)
            if progress_bin in before_apex
            else _distance_between_bins(edge_lengths, apex_bin, progress_bin)
        )
        typical_speed = _median(item["speed"] for item in comparison)
        typical_longitudinal = _median(item["longitudinal"] for item in comparison)
        typical_lateral = _median(abs(item["lateral"]) for item in comparison)
        typical_yaw = _median(
            abs(item["yaw"]) for item in comparison if item.get("yaw") is not None
        )
        result.append(
            {
                "progress_percent": _rounded(progress_bin + 0.5, 1),
                "distance_from_apex_m": _rounded(distance_from_apex, 2),
                "best_speed_kmh": _rounded(best["speed"], 2),
                "typical_speed_kmh": _rounded(typical_speed, 2),
                "best_minus_typical_speed_kmh": _rounded(
                    best["speed"] - typical_speed if typical_speed is not None else None,
                    2,
                ),
                "best_chassis_longitudinal_g": _rounded(best["longitudinal"], 3),
                "typical_chassis_longitudinal_g": _rounded(typical_longitudinal, 3),
                "best_abs_chassis_lateral_g": _rounded(abs(best["lateral"]), 3),
                "typical_abs_chassis_lateral_g": _rounded(typical_lateral, 3),
                "best_abs_yaw_rate_dps": _rounded(
                    abs(best["yaw"]) if best.get("yaw") is not None else None,
                    2,
                ),
                "typical_abs_yaw_rate_dps": _rounded(typical_yaw, 2),
                "best_minus_typical_elapsed_from_approach_s": _rounded(
                    best_elapsed - typical_elapsed
                    if best_elapsed is not None and typical_elapsed is not None
                    else None,
                    3,
                ),
                "best_minus_typical_lateral_path_m": _rounded(line_delta, 2),
            }
        )
    return result


def _coherent_line_difference(
    trace: list[dict[str, Any]],
    noise_floor: float,
    direction: str,
) -> dict[str, Any]:
    runs: list[list[dict[str, Any]]] = []
    current: list[dict[str, Any]] = []
    current_sign = 0
    for point in trace:
        delta = point.get("best_minus_typical_lateral_path_m")
        sign = 1 if isinstance(delta, (int, float)) and delta >= noise_floor else -1 if isinstance(delta, (int, float)) and delta <= -noise_floor else 0
        if sign == 0:
            if current:
                runs.append(current)
            current = []
            current_sign = 0
            continue
        if current and sign != current_sign:
            runs.append(current)
            current = []
        current.append(point)
        current_sign = sign
    if current:
        runs.append(current)
    strongest = max(runs, key=len, default=[])
    if len(strongest) < 3:
        return {
            "status": "within_gps_noise",
            "minimum_consecutive_points": 3,
            "longest_consecutive_points": len(strongest),
            "interpretation": "No sustained full-corner path difference cleared the GPS noise floor.",
        }
    shifts = [float(item["best_minus_typical_lateral_path_m"]) for item in strongest]
    median_shift = _median(shifts) or 0.0
    position = "left" if median_shift > 0.0 else "right"
    if direction in {"left", "right"}:
        inside = (direction == "left" and median_shift > 0.0) or (
            direction == "right" and median_shift < 0.0
        )
        position = "inside" if inside else "outside"
    return {
        "status": "measured_difference",
        "minimum_consecutive_points": 3,
        "longest_consecutive_points": len(strongest),
        "start_progress_percent": strongest[0]["progress_percent"],
        "end_progress_percent": strongest[-1]["progress_percent"],
        "median_shift_m": _rounded(median_shift, 2),
        "best_lap_path_position_vs_typical": position,
        "interpretation": "A sustained path difference cleared the GPS noise floor across consecutive track points.",
    }


def _driver_action_inference(
    best_onset: dict[str, Any] | None,
    slowing_distance_delta: float | None,
    slowing_time_delta: float | None,
    corner_time_delta: float | None,
    speed_deltas: dict[str, float | None],
    radio_available: bool,
) -> dict[str, Any]:
    if radio_available:
        return {
            "status": "measured_controls_available",
            "confidence": 100,
            "inference": "Use the recorded transmitter controls rather than inferring the input from speed loss.",
            "exact_input_status": "recorded",
        }
    if best_onset is None or slowing_distance_delta is None:
        return {
            "status": "data_limited",
            "confidence": 0,
            "inference": "A reliable change in the speed-reduction point was not available.",
            "exact_input_status": "not_recorded",
        }

    farther = slowing_distance_delta >= 0.50
    closer = slowing_distance_delta <= -0.50
    gained = corner_time_delta is not None and corner_time_delta < -0.01
    carried_more = (
        (speed_deltas.get("minimum") or 0.0) > 0.50
        and (speed_deltas.get("average") or 0.0) > 0.50
    )
    character = str(best_onset.get("deceleration_character") or "unknown")
    longitudinal = best_onset.get("median_chassis_longitudinal_g_to_apex")
    strong_longitudinal = isinstance(longitudinal, (int, float)) and longitudinal <= -0.12

    if farther:
        if strong_longitudinal or character == "strong":
            label = "likely_earlier_braking_or_abrupt_lift"
            action = "The speed and chassis-G pattern is most consistent with braking earlier or making a sharper earlier lift."
        elif character == "mild" and not strong_longitudinal:
            label = "likely_earlier_lift_or_short_coast"
            action = "The smooth speed loss is most consistent with an earlier lift or short coast."
        else:
            label = "likely_earlier_lift_or_light_brake"
            action = "The pattern is most consistent with an earlier lift, light brake, or a short combination of both."
        effect = (
            " It was associated with carrying more speed through the corner and gaining time, which suggests the earlier speed reduction helped the car take a cleaner, less interrupted arc."
            if gained and carried_more
            else " The exact input and its effect remain less certain because the corner-speed gain was not clearly retained."
        )
        confidence = 82 if gained and carried_more else 68
    elif closer:
        label = "likely_later_speed_reduction"
        action = "The speed trace is most consistent with lifting or braking later and closer to the corner."
        effect = (
            " It was associated with carrying more speed through the corner and gaining time."
            if gained and carried_more
            else " It did not produce a clearly retained corner-speed advantage."
        )
        confidence = 76 if gained and carried_more else 62
    else:
        label = "no_clear_change_in_speed_reduction_point"
        action = "The speed-reduction point was effectively unchanged."
        effect = " Differences in line, steering scrub, or speed recovery may explain more of the corner result."
        confidence = 58

    return {
        "status": "inferred",
        "label": label,
        "confidence": confidence,
        "measured": {
            "onset_distance_change_m": _rounded(slowing_distance_delta),
            "onset_time_change_s": _rounded(slowing_time_delta),
            "deceleration_character": character,
            "median_chassis_longitudinal_g_to_apex": _rounded(
                longitudinal if isinstance(longitudinal, (int, float)) else None,
                3,
            ),
        },
        "inference": action + effect,
        "exact_input_status": "not_recorded",
        "uncertainty": "This identifies the most likely driver action from speed, G-force, yaw, and path shape; it does not claim the transmitter command was measured.",
    }


def _session_corner_analysis(
    rows: list[dict[str, Any]],
    lap_table: list[dict[str, Any]],
    best_raw_lap: int | None,
    quality: dict[str, Any],
    selected_raw_laps: set[int] | None = None,
    comparison_scope: dict[str, Any] | None = None,
    progress_data: dict[str, Any] | None = None,
) -> dict[str, Any]:
    progress = progress_data or _session_progress_profiles(rows)
    if progress.get("status") != "measured" or best_raw_lap is None:
        return {
            "status": "data_limited",
            "reason": progress.get("reason") or "No complete reference lap was available.",
            "corners": [],
        }
    profiles = progress["profiles"]
    centerline = progress["centerline"]
    if best_raw_lap not in profiles:
        return {
            "status": "data_limited",
            "reason": "The fastest complete lap did not have enough GPS coverage.",
            "corners": [],
        }
    corners = _detected_corner_regions(centerline)
    if not corners:
        return {
            "status": "data_limited",
            "reason": "Repeatable corner zones could not be separated from the GPS/G-force profile.",
            "corners": [],
        }

    lap_durations = {
        int(item["raw_lap"]): float(item["duration_s"])
        for item in lap_table
        if item.get("phase") == "complete"
        and item.get("duration_s") is not None
        and int(item.get("raw_lap") or 0) in profiles
        and (
            selected_raw_laps is None
            or int(item.get("raw_lap") or 0) in selected_raw_laps
        )
    }
    if best_raw_lap not in lap_durations:
        best_raw_lap = min(
            lap_durations,
            key=lambda lap: lap_durations[lap],
            default=None,
        )
    if best_raw_lap is None:
        return {
            "status": "data_limited",
            "reason": "The requested lap group did not contain a complete lap with enough GPS coverage.",
            "corners": [],
            "comparison_scope": comparison_scope or {},
        }
    comparison_laps = sorted(lap for lap in lap_durations if lap != best_raw_lap)
    if not comparison_laps:
        return {
            "status": "data_limited",
            "reason": "A second comparable complete lap was not available.",
            "corners": [],
            "comparison_scope": comparison_scope or {},
        }

    translations = progress.get("translations", {})
    residuals = progress.get("residuals", {})
    selected_translation_values = [
        value for lap, value in translations.items() if lap in lap_durations
    ]
    selected_residual_values = [
        value for lap, value in residuals.items() if lap in lap_durations
    ]
    median_translation = _median(selected_translation_values)
    maximum_translation = max(selected_translation_values, default=0.0)
    median_residual = _median(selected_residual_values)
    satellites = quality.get("median_satellites")
    line_available = (
        len(lap_durations) >= 3
        and len(centerline) >= 80
        and maximum_translation <= 3.0
        and (median_residual is not None and median_residual <= 3.0)
        and (satellites is None or satellites >= 8.0)
    )
    line_noise_floor = max(0.20, min(2.25, (median_residual or 0.0) * 0.75))
    edge_lengths = progress["edge_lengths"]
    corner_results: list[dict[str, Any]] = []
    for number, corner in enumerate(corners, start=1):
        start_bin = corner["start_bin"]
        apex_bin = corner["apex_bin"]
        end_bin = corner["end_bin"]
        best_profile = profiles[best_raw_lap]
        best_duration = lap_durations[best_raw_lap]
        best_corner_time = _elapsed_between_bins(
            best_profile, start_bin, end_bin, best_duration
        )
        comparison_corner_times_by_lap = {
            lap: value
            for lap in comparison_laps
            if (
                value := _elapsed_between_bins(
                    profiles[lap], start_bin, end_bin, lap_durations[lap]
                )
            )
            is not None
        }
        comparison_corner_times = list(comparison_corner_times_by_lap.values())
        typical_corner_time = _median(comparison_corner_times)
        corner_distance_m = _distance_between_bins(
            edge_lengths, start_bin, end_bin
        )
        best_speeds = _corner_speed_summary(best_profile, start_bin, end_bin)
        best_speeds["average"] = (
            corner_distance_m / best_corner_time * 3.6
            if best_corner_time is not None and best_corner_time > 0.0
            else None
        )
        comparison_speeds = [
            {
                **_corner_speed_summary(profiles[lap], start_bin, end_bin),
                "average": corner_distance_m / corner_time * 3.6,
            }
            for lap, corner_time in comparison_corner_times_by_lap.items()
        ]
        typical_speeds = {
            key: _median(
                values[key] for values in comparison_speeds if values[key] is not None
            )
            for key in ("entry", "minimum", "exit", "average")
        }
        speed_deltas = {
            key: (
                best_speeds[key] - typical_speeds[key]
                if best_speeds[key] is not None and typical_speeds[key] is not None
                else None
            )
            for key in ("entry", "minimum", "exit", "average")
        }
        best_line_offset = _corner_line_offset(
            best_profile, centerline, apex_bin
        )
        comparison_line_offsets = [
            value
            for lap in comparison_laps
            if (value := _corner_line_offset(profiles[lap], centerline, apex_bin))
            is not None
        ]
        typical_line_offset = _median(comparison_line_offsets)
        line_shift = (
            best_line_offset - typical_line_offset
            if best_line_offset is not None and typical_line_offset is not None
            else None
        )
        best_onset = _speed_derived_slowing_onset(
            best_profile,
            corner["approach_start_bin"],
            apex_bin,
            best_duration,
            edge_lengths,
        )
        comparison_onsets = [
            onset
            for lap in comparison_laps
            if (
                onset := _speed_derived_slowing_onset(
                    profiles[lap],
                    corner["approach_start_bin"],
                    apex_bin,
                    lap_durations[lap],
                    edge_lengths,
                )
            )
            is not None
        ]
        typical_onset_distance = _median(
            onset["distance_before_apex_m"] for onset in comparison_onsets
        )
        typical_onset_time = _median(
            onset["time_before_apex_s"] for onset in comparison_onsets
        )
        slowing_delta = (
            best_onset["distance_before_apex_m"] - typical_onset_distance
            if best_onset is not None and typical_onset_distance is not None
            else None
        )
        slowing_time_delta = (
            best_onset["time_before_apex_s"] - typical_onset_time
            if best_onset is not None and typical_onset_time is not None
            else None
        )
        corner_time_delta = (
            best_corner_time - typical_corner_time
            if best_corner_time is not None and typical_corner_time is not None
            else None
        )
        line_status = "data_limited"
        if line_available and line_shift is not None:
            line_status = (
                "alignment_outlier"
                if abs(line_shift) > 3.0
                else "measured_difference"
                if abs(line_shift) >= line_noise_floor
                else "within_gps_noise"
            )
        line_position = "data_limited"
        if line_status == "within_gps_noise":
            line_position = "within_gps_noise"
        elif line_status == "alignment_outlier":
            line_position = "untrusted_alignment_outlier"
        elif line_status == "measured_difference":
            if corner["direction"] not in ("left", "right"):
                line_position = "turn_direction_unavailable"
            else:
                inside = (
                    (corner["direction"] == "left" and line_shift > 0.0)
                    or (corner["direction"] == "right" and line_shift < 0.0)
                )
                line_position = "inside" if inside else "outside"
        slowing_status = "data_limited"
        if slowing_delta is not None:
            slowing_status = (
                "earlier_or_farther_before_apex"
                if slowing_delta >= 0.50
                else "later_or_closer_to_apex"
                if slowing_delta <= -0.50
                else "within_half_metre"
            )
        comparison_profiles = [profiles[lap] for lap in comparison_laps]
        comparison_durations = [lap_durations[lap] for lap in comparison_laps]
        corner_trace = _corner_comparison_trace(
            best_profile,
            comparison_profiles,
            centerline,
            edge_lengths,
            corner["approach_start_bin"],
            apex_bin,
            end_bin,
            best_duration,
            comparison_durations,
            line_available,
        )
        full_path = (
            _coherent_line_difference(
                corner_trace, line_noise_floor, str(corner["direction"])
            )
            if line_available
            else {
                "status": "data_limited",
                "interpretation": "The GPS alignment quality was not sufficient for a full-corner line comparison.",
            }
        )
        action_inference = _driver_action_inference(
            best_onset,
            slowing_delta,
            slowing_time_delta,
            corner_time_delta,
            speed_deltas,
            float(quality.get("radio_coverage") or 0.0) >= 0.50,
        )
        corner_results.append(
            {
                "corner_id": f"corner-{number:02d}",
                "corner_number": number,
                "progress_range_percent": [
                    _rounded(start_bin + 0.5, 1),
                    _rounded(end_bin + 0.5, 1),
                ],
                "apex_progress_percent": _rounded(apex_bin + 0.5, 1),
                "wraps_start_finish": bool(corner["wraps_start_finish"]),
                "direction_from_chassis_lateral_g": corner["direction"],
                "peak_abs_lateral_g": _rounded(corner["peak_abs_lateral_g"]),
                "best_raw_lap": best_raw_lap,
                "comparison_lap_count": len(comparison_laps),
                "best_corner_time_s": _rounded(best_corner_time),
                "typical_other_laps_corner_time_s": _rounded(typical_corner_time),
                "best_minus_typical_corner_time_s": _rounded(corner_time_delta),
                "best_entry_speed_kmh": _rounded(best_speeds["entry"]),
                "typical_other_laps_entry_speed_kmh": _rounded(typical_speeds["entry"]),
                "best_minus_typical_entry_speed_kmh": _rounded(speed_deltas["entry"]),
                "best_minimum_speed_kmh": _rounded(best_speeds["minimum"]),
                "typical_other_laps_minimum_speed_kmh": _rounded(typical_speeds["minimum"]),
                "best_minus_typical_minimum_speed_kmh": _rounded(speed_deltas["minimum"]),
                "best_average_corner_speed_kmh": _rounded(best_speeds["average"]),
                "typical_other_laps_average_corner_speed_kmh": _rounded(typical_speeds["average"]),
                "best_minus_typical_average_corner_speed_kmh": _rounded(speed_deltas["average"]),
                "average_corner_speed_method": "aligned corner distance divided by elapsed corner time",
                "best_exit_speed_kmh": _rounded(best_speeds["exit"]),
                "typical_other_laps_exit_speed_kmh": _rounded(typical_speeds["exit"]),
                "best_minus_typical_exit_speed_kmh": _rounded(speed_deltas["exit"]),
                "driving_line": {
                    "status": line_status,
                    "best_lap_apex_shift_vs_typical_m": _rounded(
                        line_shift
                        if line_shift is not None and abs(line_shift) <= 3.0
                        else None
                    ),
                    "positive_shift_meaning": "left of the typical path in direction of travel",
                    "best_lap_apex_position_vs_typical": line_position,
                    "gps_noise_floor_m": _rounded(line_noise_floor),
                    "maximum_trusted_local_shift_m": 3.0,
                    "untrusted_reason": (
                        "The local path shift exceeded the three-metre alignment limit."
                        if line_status == "alignment_outlier"
                        else ""
                    ),
                    "whole_lap_translation_removed": True,
                    "full_corner_path": full_path,
                },
                "speed_derived_slowing": {
                    "status": slowing_status,
                    "best_lap_onset_progress_percent": _rounded(
                        best_onset.get("progress_percent") if best_onset else None,
                        1,
                    ),
                    "best_lap_onset_distance_before_apex_m": _rounded(
                        best_onset.get("distance_before_apex_m")
                        if best_onset
                        else None
                    ),
                    "typical_other_laps_onset_distance_before_apex_m": _rounded(
                        typical_onset_distance
                    ),
                    "best_minus_typical_onset_distance_m": _rounded(slowing_delta),
                    "best_lap_onset_time_before_apex_s": _rounded(
                        best_onset.get("time_before_apex_s") if best_onset else None
                    ),
                    "typical_other_laps_onset_time_before_apex_s": _rounded(
                        typical_onset_time
                    ),
                    "best_minus_typical_onset_time_s": _rounded(slowing_time_delta),
                    "best_lap_peak_deceleration_g_to_apex": _rounded(
                        best_onset.get("peak_speed_derived_deceleration_g_to_apex")
                        if best_onset
                        else None
                    ),
                    "best_lap_mean_deceleration_g_to_apex": _rounded(
                        best_onset.get("mean_speed_derived_deceleration_g_to_apex")
                        if best_onset
                        else None
                    ),
                    "best_lap_deceleration_character": (
                        best_onset.get("deceleration_character") if best_onset else None
                    ),
                    "best_lap_speed_loss_to_apex_kmh": _rounded(
                        best_onset.get("speed_loss_to_apex_kmh") if best_onset else None
                    ),
                    "best_lap_median_chassis_longitudinal_g_to_apex": _rounded(
                        best_onset.get("median_chassis_longitudinal_g_to_apex")
                        if best_onset
                        else None,
                        3,
                    ),
                    "typical_other_laps_mean_deceleration_g_to_apex": _rounded(
                        _median(
                            onset["mean_speed_derived_deceleration_g_to_apex"]
                            for onset in comparison_onsets
                            if onset.get("mean_speed_derived_deceleration_g_to_apex") is not None
                        )
                    ),
                    "typical_other_laps_speed_loss_to_apex_kmh": _rounded(
                        _median(
                            onset["speed_loss_to_apex_kmh"]
                            for onset in comparison_onsets
                            if onset.get("speed_loss_to_apex_kmh") is not None
                        )
                    ),
                    "interpretation": (
                        "This measures where and how speed began falling. Without transmitter data, the exact control is inferred rather than measured."
                    ),
                },
                "driver_action_inference": action_inference,
                "corner_trace": {
                    "status": "measured",
                    "grain": "one-percent track-progress points from approach through corner exit",
                    "raw_coordinates_retained": False,
                    "points": corner_trace,
                },
                "evidence_id": f"session:corner:{number:02d}:best-vs-typical",
            }
        )

    ranked = sorted(
        (
            item
            for item in corner_results
            if item.get("best_minus_typical_corner_time_s") is not None
        ),
        key=lambda item: item["best_minus_typical_corner_time_s"],
    )
    return {
        "status": "measured",
        "comparison": (
            f"selected reference lap versus the median of the other laps in {comparison_scope.get('description')}"
            if comparison_scope and comparison_scope.get("reference_raw_lap") is not None
            else f"fastest lap in {comparison_scope.get('description')} versus the median of the other selected laps"
            if comparison_scope and comparison_scope.get("selection_applied")
            else "fastest complete lap versus median of the other complete laps"
        ),
        "comparison_scope": comparison_scope or {},
        "best_raw_lap": best_raw_lap,
        "best_race_lap": next(
            (
                int(item.get("race_lap") or 0)
                for item in lap_table
                if int(item.get("raw_lap") or 0) == best_raw_lap
            ),
            None,
        ),
        "comparison_raw_laps": comparison_laps,
        "detected_corner_count": len(corner_results),
        "track_length_m": _rounded(progress.get("track_length_m")),
        "driving_line_quality": {
            "status": "measured" if line_available else "data_limited",
            "whole_lap_translation_removed": True,
            "median_translation_removed_m": _rounded(median_translation),
            "maximum_translation_removed_m": _rounded(maximum_translation),
            "median_aligned_path_residual_m": _rounded(median_residual),
            "line_conclusion_limit_m": 3.0,
            "confidence_band": (
                "strong"
                if median_residual is not None and median_residual <= 0.75
                else "cautious"
                if median_residual is not None and median_residual <= 1.50
                else "weak"
            ),
            "interpretation": (
                "GPS line differences are reported only after one whole-lap translation per lap. "
                "The correction never changes stored telemetry and a different line is not automatically a better line."
            ),
        },
        "largest_fast_lap_corner_gains": [
            {
                "corner_id": item["corner_id"],
                "apex_progress_percent": item["apex_progress_percent"],
                "best_minus_typical_corner_time_s": item[
                    "best_minus_typical_corner_time_s"
                ],
            }
            for item in ranked[:3]
            if item["best_minus_typical_corner_time_s"] < -0.01
        ],
        "largest_fast_lap_corner_losses": [
            {
                "corner_id": item["corner_id"],
                "apex_progress_percent": item["apex_progress_percent"],
                "best_minus_typical_corner_time_s": item[
                    "best_minus_typical_corner_time_s"
                ],
            }
            for item in reversed(ranked[-3:])
            if item["best_minus_typical_corner_time_s"] > 0.01
        ],
        "corners": corner_results,
        "guardrails": [
            "The fastest lap is outcome-selected; this comparison describes where it differed but does not prove why it was faster.",
            "A GPS path offset is a measured line difference, not proof that the line caused the time change.",
            "Without Sanwa, exact controls are not measured. Speed, G-force, yaw, and path shape may still support a clearly labelled likely driver-action inference.",
        ],
    }


def analyze_session(
    analytics_csv: str,
    question: str = "",
    history: list[dict[str, Any]] | None = None,
    include_scope_library: bool = False,
) -> dict[str, Any]:
    """Describe the requested lap group while retaining whole-run context."""

    rows = parse_analytics_csv(analytics_csv)
    laps = _lap_summary(rows)
    quality = _sample_quality(rows)
    lap_table = _session_lap_table(rows)
    complete = [lap for lap in lap_table if lap["phase"] == "complete"]
    question_scope = requested_lap_scope(question, history, lap_table)
    selected_raw_laps = set(question_scope["selected_raw_laps"])
    selected_complete = [
        lap for lap in complete if int(lap.get("raw_lap") or 0) in selected_raw_laps
    ]
    complete_durations = [
        lap["duration_s"] for lap in complete if lap["duration_s"] is not None
    ]
    median_duration = _median(complete_durations)
    deviations = (
        [abs(value - median_duration) for value in complete_durations]
        if median_duration is not None
        else []
    )
    best = next(
        (
            lap
            for lap in selected_complete
            if int(lap.get("raw_lap") or 0)
            == int(question_scope.get("reference_raw_lap") or -1)
        ),
        None,
    ) or min(
        selected_complete,
        key=lambda lap: lap["duration_s"] if lap["duration_s"] is not None else math.inf,
        default=None,
    )
    for lap in complete:
        duration = lap.get("duration_s")
        lap["delta_to_best_s"] = _rounded(
            duration - best["duration_s"]
            if best and duration is not None and best.get("duration_s") is not None
            else None
        )
        lap["delta_to_median_s"] = _rounded(
            duration - median_duration
            if duration is not None and median_duration is not None
            else None
        )

    radio_available = float(quality.get("radio_coverage") or 0.0) >= 0.50
    incident_candidates = _incident_candidates(rows)
    primary_candidate = (
        max(
            incident_candidates,
            key=lambda item: (
                int(item.get("confidence") or 0),
                -float(item.get("session_time_s") or 0.0),
            ),
        )
        if incident_candidates
        else None
    )
    incident_review = (
        _incident_consequence(lap_table, primary_candidate)
        if primary_candidate is not None
        else {
            "status": "no_candidate",
            "interpretation": (
                "No unusual abrupt near-stop cleared the cross-lap incident thresholds. "
                "This does not prove that the run had no contact or handling problem."
            ),
        }
    )
    progress_data = _session_progress_profiles(rows)
    corner_analysis = _session_corner_analysis(
        rows,
        lap_table,
        int(best["raw_lap"]) if best and best.get("raw_lap") is not None else None,
        quality,
        selected_raw_laps,
        question_scope,
        progress_data,
    )
    selected_durations = [
        float(lap["duration_s"])
        for lap in selected_complete
        if lap.get("duration_s") is not None
    ]
    selected_median = _median(selected_durations)
    result = {
        "contract": SESSION_REVIEW_CONTRACT,
        "formula_version": 5,
        "question_scope": question_scope,
        "requested_lap_comparison": {
            "description": question_scope["description"],
            "selected_race_laps": question_scope["selected_race_laps"],
            "selected_raw_laps": question_scope["selected_raw_laps"],
            "selected_lap_count": len(selected_complete),
            "reference_raw_lap": best.get("raw_lap") if best else None,
            "reference_race_lap": best.get("race_lap") if best else None,
            "reference_lap_time_s": best.get("duration_s") if best else None,
            "selected_median_lap_time_s": _rounded(selected_median),
            "reference_minus_selected_median_s": _rounded(
                float(best["duration_s"]) - selected_median
                if best and best.get("duration_s") is not None and selected_median is not None
                else None
            ),
        },
        "quality": {
            **quality,
            "confidence": _single_session_quality_confidence(laps, quality),
            "radio_controls_available": radio_available,
            "racebox_only_analysis_available": True,
            "all_lap_corner_analysis_available": (
                corner_analysis.get("status") == "measured"
            ),
            "driving_line_analysis_available": (
                corner_analysis.get("driving_line_quality", {}).get("status")
                == "measured"
            ),
            "speed_derived_slowing_analysis_available": (
                corner_analysis.get("status") == "measured"
            ),
            "available_without_sanwa": [
                "lap timing",
                "speed",
                "GPS position and lap progress",
                "GPS driving-line differences after whole-lap translation correction",
                "speed-derived slowing onset before detected corners",
                "chassis lateral and longitudinal G",
                "vertical G when recorded",
                "yaw rate when calibrated",
            ],
            "control_limit": (
                "Sanwa steering, throttle, and brake are available for input/output questions."
                if radio_available
                else "Sanwa controls are unavailable or too incomplete. Exact steering, throttle, and brake commands are not measured, but speed, GPS path, G-force, and yaw may support clearly labelled likely driver-action inferences."
            ),
        },
        "run_summary": {
            **laps,
            "best_raw_lap": best.get("raw_lap") if best else None,
            "lap_time_mad_s": _rounded(_median(deviations)),
            "chronological_complete_lap_times": [
                {
                    "raw_lap": lap["raw_lap"],
                    "duration_s": lap["duration_s"],
                    "delta_to_best_s": lap.get("delta_to_best_s"),
                }
                for lap in complete
            ],
        },
        "all_laps": lap_table,
        "early_vs_late": _session_trend(complete),
        "corner_analysis": corner_analysis,
        "incident_candidates": incident_candidates,
        "incident_review": incident_review,
        "braking": _braking_analysis(rows),
        "guardrails": [
            "This is one run, so it can describe patterns and correlations but cannot prove a setup-change cause.",
            "Every complete lap is included; out-lap and in-lap segments are identified separately.",
            "Corner analysis uses the driver-requested lap population when one is stated in the question or recent driver context; selecting the fastest lap does not prove that one action caused its result.",
            "GPS line differences remove only one whole-lap translation per lap, retain the original telemetry unchanged, and are withheld when correction or residual limits are exceeded.",
            "When Sanwa is missing, exact commands are not measured. A likely lift, coast, brake, or scrub explanation is allowed only when it is labelled as inference and supported by the combined speed, G-force, yaw, path, and retained-time pattern.",
            "Possible incident candidates are unusual abrupt near-stops relative to other laps at the same track position; ask the driver what happened before assigning a cause.",
            "Incident time accounting includes the incident lap and every later complete lap, using recent pre-event pace as a projection rather than claiming crash damage caused every later delta.",
            "A possible brake lockup or low-grip indicator is not confirmation; wheel speed is required.",
            "Chassis G is not direct tire load, suspension travel, or tire temperature.",
            "Late-run changes may come from tires, battery, traffic, track conditions, damage, or driver adaptation.",
        ],
    }
    if include_scope_library:
        scope_library: dict[str, Any] = {}
        available_race_laps = question_scope["available_race_laps"]
        raw_by_race = {
            int(item["race_lap"]): int(item["raw_lap"])
            for item in complete
            if int(item.get("race_lap") or 0) > 0
        }
        for limit in available_race_laps[1:-1]:
            prefix_race_laps = [value for value in available_race_laps if value <= limit]
            if len(prefix_race_laps) < 2:
                continue
            prefix_raw_laps = {raw_by_race[value] for value in prefix_race_laps}
            prefix_complete = [
                lap
                for lap in complete
                if int(lap.get("raw_lap") or 0) in prefix_raw_laps
            ]
            prefix_best = min(
                prefix_complete,
                key=lambda lap: lap["duration_s"]
                if lap.get("duration_s") is not None
                else math.inf,
                default=None,
            )
            prefix_scope = {
                **question_scope,
                "status": "requested",
                "source": "stored_prefix_scope",
                "description": f"race laps 1-{limit}",
                "matched_driver_words": f"first {limit} laps",
                "selected_race_laps": prefix_race_laps,
                "selected_raw_laps": sorted(prefix_raw_laps),
                "reference_race_lap": None,
                "reference_raw_lap": None,
                "selection_applied": True,
                "comparison_lap_count": len(prefix_race_laps),
            }
            prefix_analysis = _session_corner_analysis(
                rows,
                lap_table,
                int(prefix_best["raw_lap"])
                if prefix_best and prefix_best.get("raw_lap") is not None
                else None,
                quality,
                prefix_raw_laps,
                prefix_scope,
                progress_data,
            )
            prefix_durations = [
                float(lap["duration_s"])
                for lap in prefix_complete
                if lap.get("duration_s") is not None
            ]
            prefix_median = _median(prefix_durations)
            scope_library[str(limit)] = {
                "question_scope": prefix_scope,
                "requested_lap_comparison": {
                    "description": prefix_scope["description"],
                    "selected_race_laps": prefix_race_laps,
                    "selected_raw_laps": sorted(prefix_raw_laps),
                    "selected_lap_count": len(prefix_complete),
                    "reference_raw_lap": prefix_best.get("raw_lap") if prefix_best else None,
                    "reference_race_lap": prefix_best.get("race_lap") if prefix_best else None,
                    "reference_lap_time_s": prefix_best.get("duration_s") if prefix_best else None,
                    "selected_median_lap_time_s": _rounded(prefix_median),
                    "reference_minus_selected_median_s": _rounded(
                        float(prefix_best["duration_s"]) - prefix_median
                        if prefix_best and prefix_best.get("duration_s") is not None and prefix_median is not None
                        else None
                    ),
                },
                "corner_analysis": prefix_analysis,
            }
        result["stored_prefix_lap_scopes"] = scope_library
    return result


def apply_question_scope_to_stored_review(
    review: dict[str, Any],
    question: str,
    history: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Focus a stored processed review without retaining or reconstructing raw telemetry."""

    focused = copy.deepcopy(review)
    scope_library = focused.pop("stored_prefix_lap_scopes", {})
    lap_table = focused.get("all_laps")
    if not isinstance(lap_table, list):
        return focused
    scope = requested_lap_scope(question, history, lap_table)
    selected = scope.get("selected_race_laps", [])
    is_prefix = bool(selected) and selected == list(range(1, max(selected) + 1))
    stored = scope_library.get(str(max(selected))) if is_prefix else None
    if isinstance(stored, dict):
        stored_scope = copy.deepcopy(stored.get("question_scope", {}))
        stored_scope.update(
            {
                "source": scope.get("source"),
                "matched_driver_words": scope.get("matched_driver_words"),
                "status": scope.get("status"),
            }
        )
        focused["question_scope"] = stored_scope
        focused["requested_lap_comparison"] = copy.deepcopy(
            stored.get("requested_lap_comparison", {})
        )
        focused["corner_analysis"] = copy.deepcopy(stored.get("corner_analysis", {}))
        focused["corner_analysis"]["comparison_scope"] = stored_scope
        focused["stored_scope_resolution"] = "matched_processed_prefix_scope"
    else:
        focused["question_scope"] = scope
        focused["stored_scope_resolution"] = (
            "all_complete_laps"
            if not scope.get("selection_applied")
            else "requested_scope_not_precomputed"
        )
    return focused


def focus_session_review_for_question(
    review: dict[str, Any], question: str
) -> dict[str, Any]:
    """Keep detailed traces only for the corners needed by this model call."""

    focused = copy.deepcopy(review)
    corner_analysis = focused.get("corner_analysis")
    if not isinstance(corner_analysis, dict):
        return focused
    corners = corner_analysis.get("corners")
    if not isinstance(corners, list):
        return focused
    text = str(question or "").lower()
    detailed = bool(
        re.search(r"\b(?:all|every)\s+(?:turn|corner)s?\b", text)
        or "corner-by-corner" in text
        or "detailed" in text
    )
    requested_numbers = {
        int(value)
        for value in re.findall(r"\b(?:turn|corner|t)\s*([1-9]|1\d|20)\b", text)
    }
    if detailed:
        keep_ids = {str(item.get("corner_id")) for item in corners}
        reason = "driver_requested_detailed_corner_review"
    elif requested_numbers:
        keep_ids = {
            str(item.get("corner_id"))
            for item in corners
            if int(item.get("corner_number") or 0) in requested_numbers
        }
        reason = "driver_named_corner"
    else:
        ranked = corner_analysis.get("largest_fast_lap_corner_gains", [])
        keep_ids = {
            str(item.get("corner_id"))
            for item in ranked[:3]
            if isinstance(item, dict)
        }
        if not keep_ids:
            keep_ids = {
                str(item.get("corner_id"))
                for item in sorted(
                    corners,
                    key=lambda item: float(
                        item.get("best_minus_typical_corner_time_s") or math.inf
                    ),
                )[:3]
            }
        reason = "three_largest_corner_gains"
    for item in corners:
        if str(item.get("corner_id")) in keep_ids:
            continue
        trace = item.get("corner_trace")
        point_count = (
            len(trace.get("points", [])) if isinstance(trace, dict) else 0
        )
        item["corner_trace"] = {
            "status": "summarized_not_focused",
            "point_count": point_count,
            "interpretation": "The summary remains available; the detailed trace was omitted from this model call because another corner was requested.",
        }
    focused["trace_focus"] = {
        "reason": reason,
        "detailed_corner_ids": sorted(keep_ids),
        "other_corner_summaries_retained": True,
        "raw_coordinates_retained": False,
    }
    return focused


def analyze_pair(previous_csv: str, current_csv: str) -> dict[str, Any]:
    previous = parse_analytics_csv(previous_csv)
    current = parse_analytics_csv(current_csv)
    track = _track_compatibility(previous, current)
    if track["status"] == "incompatible":
        raise ValueError(
            "previous and current telemetry do not describe the same track layout"
        )
    previous_laps = _lap_summary(previous)
    current_laps = _lap_summary(current)
    previous_quality = _sample_quality(previous)
    current_quality = _sample_quality(current)
    previous_top3 = previous_laps["top3_median_s"]
    current_top3 = current_laps["top3_median_s"]
    lap_delta = (
        current_top3 - previous_top3
        if current_top3 is not None and previous_top3 is not None
        else None
    )
    confidence = _quality_confidence(
        previous_laps, current_laps, previous_quality, current_quality
    )
    return {
        "contract": ANALYTICS_CONTRACT,
        "formula_version": 3,
        "quality": {
            "confidence": confidence,
            "previous": previous_quality,
            "current": current_quality,
            "track_compatibility": track,
            "minimum_complete_laps_for_repeatability": 3,
        },
        "lap_time": {
            "previous": previous_laps,
            "current": current_laps,
            "current_minus_previous_top3_median_s": _rounded(lap_delta),
            "evidence_id": "outcome:lap-time:top3-median",
            "interpretation": (
                "Negative is faster. A dynamics improvement is not called a reliable gain when "
                "the repeated whole-run outcome is slower or the difference is within normal spread."
            ),
        },
        "lateral_response": _lateral_analysis(previous, current),
        "forward_bite": _forward_analysis(previous, current),
        "straight_speed": _straight_analysis(previous, current),
        "corner_balance": _corner_balance_analysis(previous, current),
        "overdriving": _overdriving_analysis(previous, current, lap_delta),
        "chassis_roll": _chassis_roll_analysis(previous, current),
        "roll_rate": _roll_rate_analysis(previous, current),
        "braking": {
            "previous": _braking_analysis(previous),
            "current": _braking_analysis(current),
        },
        "decision_guardrails": [
            "One previous/current pair shows association, not causation.",
            "Prefer at least three complete laps in each run and repeat the A/B setting sequence.",
            "Do not call a local response increase a gain if top-three lap time became slower.",
            "Compare track/tire temperature, tire set and run count, sauce, warmer, battery, traffic, damage, and driver notes.",
            "Matched dynamics samples must share physical lap-progress, speed, and steering-input bins.",
            "Chassis G is not measured tire load. A brake-lock claim requires wheel speed.",
        ],
    }
