#!/usr/bin/env python3
"""Deterministic trust gates for the RaceBox adaptive model router."""

from __future__ import annotations

from typing import Any

ROUTER_SPEC = {
    "id": "brain",
    "route": "openclaw/racebox-crew-chief-brain",
    "model": "openai/gpt-5.6-sol",
    "thinking": "xhigh",
    "purpose": "Classify the task and select the least-expensive trustworthy lane.",
}

SINGLE_AGENT_TEST_SPEC = {
    "id": "single-agent-test",
    "route": "openclaw/racebox-crew-chief-single-agent-test",
    "model": "openai/gpt-5.6-sol",
    "thinking": "xhigh",
    "purpose": (
        "Answer directly from the same bounded evidence and coaching rules, "
        "without a separate routing-model call."
    ),
}

LANE_SPECS = {
    "language": {
        "route": "openclaw/racebox-crew-chief-language",
        "model": "openai/gpt-5.6-luna",
        "thinking": "low",
        "purpose": "Plain-language wording, definitions, and short summaries.",
    },
    "explanation": {
        "route": "openclaw/racebox-crew-chief-explanation",
        "model": "openai/gpt-5.6-terra",
        "thinking": "medium",
        "purpose": "Explain clear deterministic results without difficult correlation.",
    },
    "analysis": {
        "route": "openclaw/racebox-crew-chief-analysis",
        "model": "openai/gpt-5.6-terra",
        "thinking": "high",
        "purpose": "Routine setup analysis with ordinary confounds and trade-offs.",
    },
    "correlation": {
        "route": "openclaw/racebox-crew-chief-correlation",
        "model": "openai/gpt-5.6-sol",
        "thinking": "xhigh",
        "purpose": "Difficult correlations, consequential gains, and conflicting evidence.",
    },
    "ultra": {
        "route": "openclaw/racebox-crew-chief-ultra",
        "model": "openai/gpt-5.6-sol",
        "thinking": "ultra",
        "purpose": "Rare high-trust, high-complexity correlation review.",
    },
}

LANE_ORDER = ["language", "explanation", "analysis", "correlation", "ultra"]

LANGUAGE_MARKERS = (
    "explain the term",
    "define the term",
    "what is meant by",
    "plain language",
    "layman",
    "rewrite",
    "reword",
    "summarize only",
)
CORRELATION_MARKERS = (
    "cause",
    "caused",
    "correlation",
    "reliable",
    "real gain",
    "setup change",
    "trade-off",
    "tradeoff",
    "consequence",
    "consequential",
    "why",
    "good or bad",
    "better or worse",
    "what did i do better",
    "where did i do better",
    "how did i do better",
    "did i do better",
    "driving line",
    "racing line",
    "line choice",
    "corner by corner",
    "corner comparison",
    "slowing sooner",
    "slowing earlier",
    "slowing later",
    "slow down sooner",
    "slow down earlier",
    "slow down later",
    "deceleration",
    "fastest lap",
    "crash",
    "impact",
    "hit the wall",
    "hit a wall",
    "damage",
    "damaged",
    "pre-impact",
    "post-impact",
    "before and after",
    "how much time",
    "time did i lose",
)


def _number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _nested(value: Any, *keys: str, default: Any = None) -> Any:
    current = value
    for key in keys:
        if not isinstance(current, dict):
            return default
        current = current.get(key)
    return default if current is None else current


def _meaningful(value: Any) -> bool:
    if value is None:
        return False
    if isinstance(value, str):
        return bool(value.strip())
    return True


def _condition_confounds(
    previous_context: dict[str, Any], current_context: dict[str, Any]
) -> list[str]:
    previous = _nested(previous_context, "conditions", default={})
    current = _nested(current_context, "conditions", default={})
    if not isinstance(previous, dict) or not isinstance(current, dict):
        return []

    labels: list[str] = []
    numeric_fields = {
        "ambient_temperature_c": ("ambient temperature", 2.0),
        "track_temperature_c": ("track temperature", 2.0),
        "tire_runs_before": ("tire run count", 1.0),
        "sauce_minutes_before": ("sauce timing", 2.0),
        "tire_warmer_minutes": ("tire-warmer time", 2.0),
        "tire_warmer_temperature_c": ("tire-warmer temperature", 3.0),
        "battery_voltage": ("battery voltage", 0.2),
    }
    for key, (label, threshold) in numeric_fields.items():
        before = _number(previous.get(key))
        after = _number(current.get(key))
        if before is not None and after is not None and abs(after - before) >= threshold:
            labels.append(label)

    text_fields = {
        "track_condition": "track condition",
        "tire_set_id": "tire set",
        "tire_compound": "tire compound",
        "sauce_compound": "sauce compound",
        "battery_pack": "battery pack",
    }
    for key, label in text_fields.items():
        before = str(previous.get(key) or "").strip().casefold()
        after = str(current.get(key) or "").strip().casefold()
        if before and after and before != after:
            labels.append(label)

    for name, context in (("previous", previous_context), ("current", current_context)):
        completed = _number(_nested(context, "pre_run_checklist", "completed"))
        total = _number(_nested(context, "pre_run_checklist", "total"))
        if completed is not None and total and completed < total:
            labels.append(f"{name} checklist incomplete")

    return sorted(set(labels))


def _signal_direction(value: float | None, threshold: float) -> str:
    if value is None:
        return "data_limited"
    if value >= threshold:
        return "positive"
    if value <= -threshold:
        return "negative"
    return "within_noise"


def assess_task(
    request_value: dict[str, Any], analytics: dict[str, Any] | None = None
) -> dict[str, Any]:
    """Build model-independent routing facts and the hard ultra eligibility gate."""

    question = str(request_value.get("question") or "").strip()
    lowered = question.casefold()
    language_only = any(marker in lowered for marker in LANGUAGE_MARKERS) or (
        "what does" in lowered and "mean" in lowered
    )
    asks_correlation = any(marker in lowered for marker in CORRELATION_MARKERS)
    mode = str(request_value.get("mode") or "lap")

    if mode == "session_chat" and isinstance(analytics, dict):
        quality = analytics.get("quality", {})
        confidence = int(max(0, min(100, _number(quality.get("confidence")) or 0)))
        complete_laps = int(
            _number(_nested(analytics, "run_summary", "complete_laps")) or 0
        )
        candidates = analytics.get("incident_candidates", [])
        incident_candidate_count = len(candidates) if isinstance(candidates, list) else 0
        incident_context_status = str(
            _nested(analytics, "incident_context", "status", default="none_detected")
        )
        incident_consequence_status = str(
            _nested(analytics, "incident_review", "status", default="no_candidate")
        )
        detected_corner_count = int(
            _number(_nested(analytics, "corner_analysis", "detected_corner_count"))
            or 0
        )
        driving_line_available = (
            str(
                _nested(
                    analytics,
                    "corner_analysis",
                    "driving_line_quality",
                    "status",
                    default="data_limited",
                )
            )
            == "measured"
        )
        minimum_lane = "language" if language_only else (
            "correlation"
            if asks_correlation or incident_candidate_count > 0
            else "analysis"
        )
        return {
            "mode": mode,
            "question": question,
            "language_only": language_only,
            "asks_correlation": asks_correlation,
            "quality_confidence": confidence,
            "evidence_trust": (
                "high" if confidence >= 80 else "medium" if confidence >= 60 else "low"
            ),
            "complete_laps": complete_laps,
            "radio_controls_available": bool(
                quality.get("radio_controls_available")
            ),
            "racebox_only_analysis_available": bool(
                quality.get("racebox_only_analysis_available")
            ),
            "incident_candidate_count": incident_candidate_count,
            "incident_context_status": incident_context_status,
            "incident_consequence_status": incident_consequence_status,
            "detected_corner_count": detected_corner_count,
            "driving_line_analysis_available": driving_line_available,
            "minimum_lane": minimum_lane,
            "maximum_lane": "correlation",
            "ultra_eligible": False,
            "ultra_denial_reasons": [
                "Ultra is reserved for difficult repeatable previous/current correlations; one-run chat is capped at the correlation lane."
            ],
            "complexity_score": (
                (1 if asks_correlation else 0)
                + (2 if incident_candidate_count > 0 else 0)
                + (1 if incident_context_status == "needs_driver_context" else 0)
                + (1 if asks_correlation and detected_corner_count > 0 else 0)
            ),
            "material_confounds": (
                ["possible incident needs driver context"]
                if incident_context_status == "needs_driver_context"
                else ["incident-associated change is not causal proof"]
                if incident_candidate_count > 0
                else []
            ),
            "deterministic_math_precedes_model": True,
        }

    if mode != "race_day" or not isinstance(analytics, dict):
        evidence = request_value.get("evidence")
        confidence = _number(_nested(evidence, "confidence", default=None))
        if confidence is None:
            confidence = _number(_nested(evidence, "quality", "confidence", default=0))
        confidence = max(0, min(100, int(confidence or 0)))
        minimum_lane = "language" if language_only else (
            "correlation" if asks_correlation else "analysis"
        )
        return {
            "mode": mode,
            "question": question,
            "language_only": language_only,
            "asks_correlation": asks_correlation,
            "quality_confidence": confidence,
            "evidence_trust": (
                "high" if confidence >= 80 else "medium" if confidence >= 60 else "low"
            ),
            "minimum_lane": minimum_lane,
            "maximum_lane": "correlation",
            "ultra_eligible": False,
            "ultra_denial_reasons": [
                "Ultra requires a repeatable previous/current race-day comparison."
            ],
            "complexity_score": 0,
            "material_confounds": [],
            "deterministic_math_precedes_model": True,
        }

    quality = _nested(analytics, "quality", default={})
    confidence = int(max(0, min(100, _number(_nested(quality, "confidence")) or 0)))
    previous_laps = int(
        _number(_nested(analytics, "lap_time", "previous", "complete_laps")) or 0
    )
    current_laps = int(
        _number(_nested(analytics, "lap_time", "current", "complete_laps")) or 0
    )
    minimum_laps = min(previous_laps, current_laps)
    track_status = str(
        _nested(quality, "track_compatibility", "status", default="data_limited")
    )

    lateral_status = str(
        _nested(analytics, "lateral_response", "status", default="data_limited")
    )
    forward_status = str(
        _nested(analytics, "forward_bite", "status", default="data_limited")
    )
    lateral_delta = _number(
        _nested(analytics, "lateral_response", "matched_lateral_response_delta_g")
    )
    forward_delta = _number(
        _nested(
            analytics,
            "forward_bite",
            "matched_speed_derived_acceleration_delta_g",
        )
    )
    lateral_direction = (
        _signal_direction(lateral_delta, 0.02)
        if lateral_status == "measured"
        else "data_limited"
    )
    forward_direction = (
        _signal_direction(forward_delta, 0.015)
        if forward_status == "measured"
        else "data_limited"
    )
    measured_metrics = sum(
        status == "measured" for status in (lateral_status, forward_status)
    )

    lap_delta = _number(
        _nested(
            analytics,
            "lap_time",
            "current_minus_previous_top3_median_s",
        )
    )
    previous_spread = _number(
        _nested(analytics, "lap_time", "previous", "top3_range_s")
    )
    current_spread = _number(
        _nested(analytics, "lap_time", "current", "top3_range_s")
    )
    timing_noise = max(
        0.05,
        (previous_spread or 0.0) / 2.0,
        (current_spread or 0.0) / 2.0,
    )
    if lap_delta is None:
        timing_direction = "data_limited"
    elif lap_delta < -timing_noise:
        timing_direction = "faster"
    elif lap_delta > timing_noise:
        timing_direction = "slower"
    else:
        timing_direction = "within_run_spread"

    local_directions = {
        value for value in (lateral_direction, forward_direction)
        if value in {"positive", "negative"}
    }
    contradiction = (
        timing_direction in {"slower", "within_run_spread"}
        and "positive" in local_directions
    ) or (
        timing_direction == "faster" and "negative" in local_directions
    ) or len(local_directions) > 1

    previous_braking = int(
        _number(
            _nested(
                analytics,
                "braking",
                "previous",
                "possible_lockup_or_low_grip_indicators",
            )
        )
        or 0
    )
    current_braking = int(
        _number(
            _nested(
                analytics,
                "braking",
                "current",
                "possible_lockup_or_low_grip_indicators",
            )
        )
        or 0
    )
    braking_changed = previous_braking != current_braking

    previous_context = request_value.get("previous", {}).get("context", {})
    current_context = request_value.get("current", {}).get("context", {})
    confounds = _condition_confounds(previous_context, current_context)

    complexity = 0
    complexity += 2 if contradiction else 0
    complexity += 2 if len(confounds) >= 2 else 1 if confounds else 0
    complexity += 1 if measured_metrics >= 2 else 0
    complexity += 1 if asks_correlation else 0
    complexity += 1 if braking_changed else 0
    complexity += 1 if timing_direction == "within_run_spread" else 0

    denial: list[str] = []
    if confidence < 85:
        denial.append("Deterministic evidence confidence is below 85.")
    if minimum_laps < 3:
        denial.append("Each run needs at least three complete laps.")
    if track_status != "compatible":
        denial.append("The two runs do not have verified compatible track shape.")
    if measured_metrics < 2:
        denial.append("Fewer than two matched vehicle-dynamics metrics are measured.")
    if not asks_correlation:
        denial.append("The question does not require a difficult correlation judgment.")
    if complexity < 6:
        denial.append("The correlation complexity score is below 6.")
    ultra_eligible = not denial

    # Terra/high passed the representative consequential-gain eval. Keep it as
    # the floor for substantive race-day work and let the xhigh brain escalate
    # only when the full conflict/confound pattern warrants Sol.
    minimum_lane = "language" if language_only else "analysis"
    return {
        "mode": mode,
        "question": question,
        "language_only": language_only,
        "asks_correlation": asks_correlation,
        "quality_confidence": confidence,
        "evidence_trust": (
            "high" if confidence >= 80 else "medium" if confidence >= 60 else "low"
        ),
        "complete_laps_each_minimum": minimum_laps,
        "track_status": track_status,
        "measured_dynamics_metrics": measured_metrics,
        "timing_direction": timing_direction,
        "timing_delta_s": lap_delta,
        "timing_noise_floor_s": round(timing_noise, 4),
        "lateral_direction": lateral_direction,
        "forward_direction": forward_direction,
        "braking_indicator_count_changed": braking_changed,
        "contradictory_or_consequential_evidence": contradiction,
        "material_confounds": confounds,
        "complexity_score": complexity,
        "minimum_lane": minimum_lane,
        "maximum_lane": "ultra" if ultra_eligible else "correlation",
        "ultra_eligible": ultra_eligible,
        "ultra_denial_reasons": denial,
        "deterministic_math_precedes_model": True,
    }


def deterministic_fallback_lane(assessment: dict[str, Any]) -> str:
    """Choose a safe lane when the xhigh brain is unavailable; never choose Ultra."""

    minimum = str(assessment.get("minimum_lane") or "analysis")
    if minimum == "language":
        return "language"
    if minimum == "correlation":
        return "correlation"
    if int(assessment.get("quality_confidence") or 0) < 60:
        return "explanation"
    return "analysis"


def normalize_brain_decision(
    value: Any, assessment: dict[str, Any]
) -> dict[str, Any]:
    """Validate the brain output and enforce deterministic min/max trust gates."""

    supplied = value if isinstance(value, dict) else {}
    requested = str(supplied.get("lane") or "").strip().casefold()
    if requested not in LANE_SPECS:
        requested = deterministic_fallback_lane(assessment)

    minimum = str(assessment.get("minimum_lane") or "analysis")
    maximum = str(assessment.get("maximum_lane") or "correlation")
    requested_index = LANE_ORDER.index(requested)
    minimum_index = LANE_ORDER.index(minimum)
    maximum_index = LANE_ORDER.index(maximum)
    selected_index = max(minimum_index, min(requested_index, maximum_index))
    selected = LANE_ORDER[selected_index]

    try:
        confidence = max(0, min(100, int(supplied.get("confidence", 0))))
    except (TypeError, ValueError):
        confidence = 0
    reasons = supplied.get("reasons")
    if not isinstance(reasons, list):
        reasons = []
    reasons = [str(reason)[:400] for reason in reasons[:5] if str(reason).strip()]
    if requested == "ultra" and selected != "ultra":
        reasons.append("Ultra was denied by the deterministic evidence gate.")

    return {
        "task_type": str(supplied.get("task_type") or "telemetry_review")[:80],
        "requested_lane": requested,
        "selected_lane": selected,
        "confidence": confidence,
        "reasons": reasons,
        "ultra_eligible": bool(assessment.get("ultra_eligible")),
        "ultra_used": selected == "ultra",
    }
