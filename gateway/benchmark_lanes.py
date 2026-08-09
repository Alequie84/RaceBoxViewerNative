#!/usr/bin/env python3
"""Small live eval for choosing RaceBox intelligence lanes on OpenClaw."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any

import gateway
from intelligence_router import LANE_ORDER, LANE_SPECS

TASKS = [
    {
        "id": "plain_language",
        "candidates": ["language", "explanation"],
        "expected_verdicts": {"inconclusive", "data_limited"},
        "required_ids": {"dynamics:lateral:matched-speed-steering"},
        "required_concepts": ["not direct tire load", "not proof of grip"],
        "payload": {
            "question": "Explain this result to a club racer in ordinary language.",
            "deterministic_vehicle_dynamics_analytics": {
                "quality": {"confidence": 82},
                "lateral_response": {
                    "status": "measured",
                    "matched_lateral_response_delta_g": 0.04,
                    "evidence_id": "dynamics:lateral:matched-speed-steering",
                    "interpretation": (
                        "Positive means more chassis lateral response at matched speed "
                        "and steering input. It is not direct tire load or proof of grip."
                    ),
                },
            },
        },
    },
    {
        "id": "ordinary_analysis",
        "candidates": ["explanation", "analysis"],
        "expected_verdicts": {"supported", "mixed"},
        "required_ids": {
            "outcome:lap-time:top3-median",
            "dynamics:lateral:matched-speed-steering",
        },
        "required_concepts": ["association", "repeat"],
        "payload": {
            "question": "Did the current run show a useful improvement?",
            "previous_context": {"conditions": {"track_temperature_c": 28}},
            "current_context": {"conditions": {"track_temperature_c": 29}},
            "deterministic_vehicle_dynamics_analytics": {
                "quality": {"confidence": 91},
                "lap_time": {
                    "current_minus_previous_top3_median_s": -0.18,
                    "previous": {"complete_laps": 3, "top3_range_s": 0.10},
                    "current": {"complete_laps": 3, "top3_range_s": 0.08},
                    "evidence_id": "outcome:lap-time:top3-median",
                },
                "lateral_response": {
                    "status": "measured",
                    "matched_lateral_response_delta_g": 0.035,
                    "evidence_id": "dynamics:lateral:matched-speed-steering",
                },
                "forward_bite": {
                    "status": "measured",
                    "matched_speed_derived_acceleration_delta_g": 0.021,
                    "evidence_id": "dynamics:forward-bite:matched-full-throttle",
                },
            },
        },
    },
    {
        "id": "consequential_correlation",
        "candidates": ["analysis", "correlation"],
        "expected_verdicts": {"not_supported", "mixed"},
        "required_ids": {
            "outcome:lap-time:top3-median",
            "dynamics:forward-bite:matched-full-throttle",
        },
        "required_concepts": ["slower", "not", "repeat"],
        "payload": {
            "question": (
                "Is the stronger exit a reliable setup gain, or a consequence of "
                "losing time earlier?"
            ),
            "previous_context": {"conditions": {"tire_runs_before": 2}},
            "current_context": {"conditions": {"tire_runs_before": 5}},
            "deterministic_vehicle_dynamics_analytics": {
                "quality": {"confidence": 90},
                "lap_time": {
                    "current_minus_previous_top3_median_s": 0.22,
                    "previous": {"complete_laps": 3, "top3_range_s": 0.10},
                    "current": {"complete_laps": 3, "top3_range_s": 0.12},
                    "evidence_id": "outcome:lap-time:top3-median",
                    "interpretation": "Positive is slower.",
                },
                "lateral_response": {
                    "status": "measured",
                    "matched_lateral_response_delta_g": 0.05,
                    "evidence_id": "dynamics:lateral:matched-speed-steering",
                },
                "forward_bite": {
                    "status": "measured",
                    "matched_speed_derived_acceleration_delta_g": 0.03,
                    "evidence_id": "dynamics:forward-bite:matched-full-throttle",
                },
                "decision_guardrails": [
                    "A local improvement is not a gain when the whole run is slower.",
                    "One A/B pair is association, not causation.",
                ],
            },
        },
    },
    {
        "id": "high_trust_ambiguous_correlation",
        "candidates": ["correlation", "ultra"],
        "expected_verdicts": {"mixed", "inconclusive", "not_supported"},
        "required_ids": {
            "outcome:lap-time:top3-median",
            "dynamics:lateral:matched-speed-steering",
            "dynamics:braking:possible-lockup-low-grip",
        },
        "required_concepts": ["temperature", "tire", "battery", "a/b"],
        "payload": {
            "question": (
                "Decide whether the setup caused a reliable gain after accounting for "
                "the contradictory repeated-lap and dynamics evidence."
            ),
            "previous_context": {
                "conditions": {
                    "track_temperature_c": 24,
                    "tire_runs_before": 1,
                    "battery_pack": "A",
                }
            },
            "current_context": {
                "conditions": {
                    "track_temperature_c": 33,
                    "tire_runs_before": 5,
                    "battery_pack": "B",
                }
            },
            "deterministic_vehicle_dynamics_analytics": {
                "quality": {"confidence": 94},
                "lap_time": {
                    "current_minus_previous_top3_median_s": -0.04,
                    "previous": {"complete_laps": 4, "top3_range_s": 0.12},
                    "current": {"complete_laps": 4, "top3_range_s": 0.14},
                    "evidence_id": "outcome:lap-time:top3-median",
                },
                "lateral_response": {
                    "status": "measured",
                    "matched_lateral_response_delta_g": 0.045,
                    "evidence_id": "dynamics:lateral:matched-speed-steering",
                },
                "forward_bite": {
                    "status": "measured",
                    "matched_speed_derived_acceleration_delta_g": -0.025,
                    "evidence_id": "dynamics:forward-bite:matched-full-throttle",
                },
                "braking": {
                    "previous": {"possible_lockup_or_low_grip_indicators": 0},
                    "current": {
                        "possible_lockup_or_low_grip_indicators": 2,
                        "evidence_id": "dynamics:braking:possible-lockup-low-grip",
                    },
                },
                "decision_guardrails": [
                    "One A/B pair is association, not causation.",
                    "Require A/B/A or B/A/B for stronger causal confidence.",
                ],
            },
        },
    },
    {
        "id": "setup_memory_recommendation",
        "baseline_lane": "correlation",
        "candidates": ["correlation", "language", "explanation", "analysis"],
        "expected_verdicts": {"not_supported", "mixed"},
        "required_ids": {
            "outcome:lap-time:top3-median",
            "dynamics:forward-bite:matched-full-throttle",
            "setup-result-rear-spring-01",
        },
        "required_concepts": ["slower", "prior", "repeat"],
        "payload": {
            "question": (
                "The car felt like it rotated more, but I still need rear grip on power. "
                "Based on this run and the matching result from earlier today, should I "
                "keep this rear spring change?"
            ),
            "previous_context": {
                "conditions": {
                    "track_temperature_c": 28,
                    "tire_set_id": "Set 3",
                    "tire_runs_before": 3,
                    "battery_pack": "A",
                }
            },
            "current_context": {
                "conditions": {
                    "track_temperature_c": 29,
                    "tire_set_id": "Set 3",
                    "tire_runs_before": 4,
                    "battery_pack": "A",
                }
            },
            "relevant_prior_setup_results": [
                {
                    "record_id": "setup-result-rear-spring-01",
                    "track_name": "RC Raceway",
                    "setup_change": "Rear spring 2.6 to 2.8",
                    "driver_result": "More rotation, but nervous when power was applied.",
                    "verdict": "mixed",
                    "confidence": 82,
                    "summary": (
                        "The car rotated more, but forward acceleration and the retained "
                        "lap result were worse."
                    ),
                    "evidence_summary": {
                        "quality_confidence": 86,
                        "lap_time_delta_s": 0.15,
                        "lateral_response_delta_g": 0.04,
                        "forward_bite_delta_g": -0.02,
                    },
                }
            ],
            "prior_results_are_historical_associations": True,
            "deterministic_vehicle_dynamics_analytics": {
                "quality": {"confidence": 92},
                "lap_time": {
                    "current_minus_previous_top3_median_s": 0.12,
                    "previous": {"complete_laps": 4, "top3_range_s": 0.09},
                    "current": {"complete_laps": 4, "top3_range_s": 0.10},
                    "evidence_id": "outcome:lap-time:top3-median",
                    "interpretation": "Positive is slower.",
                },
                "lateral_response": {
                    "status": "measured",
                    "matched_lateral_response_delta_g": 0.04,
                    "evidence_id": "dynamics:lateral:matched-speed-steering",
                },
                "forward_bite": {
                    "status": "measured",
                    "matched_speed_derived_acceleration_delta_g": -0.018,
                    "evidence_id": "dynamics:forward-bite:matched-full-throttle",
                },
                "decision_guardrails": [
                    "A local response increase is not a gain when the whole run is slower.",
                    "Historical results remain associations and must be repeated A/B/A.",
                ],
            },
        },
    },
]


def grade(task: dict[str, Any], report: dict[str, Any]) -> dict[str, Any]:
    text = json.dumps(report, ensure_ascii=True).casefold()
    evidence_ids = {
        evidence_id
        for observation in report.get("observations", [])
        for evidence_id in observation.get("evidence_ids", [])
    }
    checks = {
        "valid_verdict": report.get("verdict") in task["expected_verdicts"],
        "required_evidence_ids": task["required_ids"].issubset(evidence_ids),
        "causality_boundary": any(
            marker in str(report.get("causality_note") or "").casefold()
            for marker in ("not prove", "does not prove", "association", "cannot prove")
        ),
        "next_controlled_test": bool(str(report.get("next_test") or "").strip()),
        "required_concepts": all(
            concept.casefold() in text for concept in task["required_concepts"]
        ),
        "no_confirmed_tire_load": "confirmed tire load" not in text,
        "no_confirmed_lockup": "confirmed lockup" not in text,
    }
    score = round(100.0 * sum(checks.values()) / len(checks))
    return {
        "score": score,
        "passed": score >= 86 and all(
            checks[key]
            for key in (
                "valid_verdict",
                "causality_boundary",
                "next_controlled_test",
                "no_confirmed_tire_load",
                "no_confirmed_lockup",
            )
        ),
        "checks": checks,
    }


def run_task(task: dict[str, Any], lane: str) -> dict[str, Any]:
    started = time.monotonic()
    raw = gateway.openclaw_completion(
        LANE_SPECS[lane]["route"],
        [
            {"role": "system", "content": gateway.SYSTEM_PROMPT},
            {
                "role": "user",
                "content": json.dumps(
                    task["payload"], separators=(",", ":"), ensure_ascii=True
                ),
            },
        ],
        gateway.WORKER_TIMEOUT,
    )
    latency_ms = round((time.monotonic() - started) * 1000.0, 1)
    report = gateway.normalize_report(raw)
    return {
        "task": task["id"],
        "lane": lane,
        "model": LANE_SPECS[lane]["model"],
        "thinking": LANE_SPECS[lane]["thinking"],
        "latency_ms": latency_ms,
        "grade": grade(task, report),
        "report": report,
    }


def baseline_agreement(
    task: dict[str, Any],
    baseline_report: dict[str, Any],
    candidate_report: dict[str, Any],
) -> dict[str, Any]:
    baseline_ids = {
        evidence_id
        for observation in baseline_report.get("observations", [])
        for evidence_id in observation.get("evidence_ids", [])
    }
    candidate_ids = {
        evidence_id
        for observation in candidate_report.get("observations", [])
        for evidence_id in observation.get("evidence_ids", [])
    }
    checks = {
        "same_verdict": candidate_report.get("verdict")
        == baseline_report.get("verdict"),
        "confidence_within_15": abs(
            int(candidate_report.get("confidence", 0))
            - int(baseline_report.get("confidence", 0))
        )
        <= 15,
        "same_required_evidence": task["required_ids"].issubset(candidate_ids),
        "baseline_evidence_not_lost": baseline_ids.issubset(candidate_ids),
        "same_causality_boundary": any(
            marker in str(candidate_report.get("causality_note") or "").casefold()
            for marker in ("not prove", "does not prove", "association", "cannot prove")
        ),
    }
    return {
        "passed": all(checks.values()),
        "checks": checks,
        "baseline_lane": task.get("baseline_lane"),
    }


def recommendation_for(
    task: dict[str, Any], results: list[dict[str, Any]]
) -> dict[str, Any]:
    relevant = [item for item in results if item["task"] == task["id"]]
    passing = [
        item
        for item in relevant
        if item["grade"]["passed"]
        and item.get("baseline_agreement", {}).get("passed", True)
    ]
    if passing:
        winner = min(
            passing,
            key=lambda item: (
                LANE_ORDER.index(item["lane"]),
                item["latency_ms"],
            ),
        )
        reason = "Lowest intelligence lane that passed every mandatory trust check."
    else:
        winner = max(
            relevant,
            key=lambda item: (item["grade"]["score"], -item["latency_ms"]),
        )
        reason = "No candidate passed every check; highest measured score selected."
    return {
        "task": task["id"],
        "recommended_lane": winner["lane"],
        "model": winner["model"],
        "thinking": winner["thinking"],
        "score": winner["grade"]["score"],
        "reason": reason,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        default=str(gateway.STATE_DIRECTORY / "last-lane-benchmark.json"),
    )
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Skip the ordinary-analysis pair; retain language, correlation, and Ultra checks.",
    )
    args = parser.parse_args()
    selected_tasks = [
        task for task in TASKS
        if not args.quick or task["id"] != "ordinary_analysis"
    ]
    results: list[dict[str, Any]] = []
    for task in selected_tasks:
        candidates = list(task["candidates"])
        if task.get("baseline_lane") in candidates:
            baseline_lane = task["baseline_lane"]
            candidates = [baseline_lane] + [
                lane for lane in candidates if lane != baseline_lane
            ]
        task_results: list[dict[str, Any]] = []
        for lane in candidates:
            print(f"RUN task={task['id']} lane={lane}", flush=True)
            try:
                result = run_task(task, lane)
            except Exception as error:
                result = {
                    "task": task["id"],
                    "lane": lane,
                    "model": LANE_SPECS[lane]["model"],
                    "thinking": LANE_SPECS[lane]["thinking"],
                    "latency_ms": None,
                    "grade": {"score": 0, "passed": False, "checks": {}},
                    "error": str(error)[:1000],
                }
            results.append(result)
            task_results.append(result)
            print(
                f"DONE task={task['id']} lane={lane} "
                f"score={result['grade']['score']} "
                f"passed={result['grade']['passed']} "
                f"latency_ms={result.get('latency_ms')}",
                flush=True,
            )
        if task.get("baseline_lane"):
            baseline = next(
                (
                    item
                    for item in task_results
                    if item["lane"] == task["baseline_lane"] and "report" in item
                ),
                None,
            )
            if baseline is not None:
                for item in task_results:
                    if "report" in item:
                        item["baseline_agreement"] = baseline_agreement(
                            task, baseline["report"], item["report"]
                        )

    recommendations = [
        recommendation_for(task, results) for task in selected_tasks
    ]
    output = {
        "contract": "racebox-intelligence-benchmark-v1",
        "generated_at_epoch_s": int(time.time()),
        "results": results,
        "recommendations": recommendations,
    }
    target = Path(args.output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(target), "recommendations": recommendations}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
