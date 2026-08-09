#!/usr/bin/env python3
"""Compare the live adaptive and single-Sol Crew Chief paths without saving chat."""

from __future__ import annotations

import argparse
import json
import re
import time
from pathlib import Path
from typing import Any

import gateway
from companion_store import CompanionStore


DEFAULT_QUESTION = (
    "What did I do better in this run, including the driving line? I did not "
    "record transmitter telemetry. Explain what I did best and how I can go "
    "faster corner by corner, with steering and throttle suggestions to test."
)


def _report_text(report: dict[str, Any]) -> str:
    body = report.get("report", {})
    chunks = [
        str(body.get("summary") or ""),
        str(body.get("next_test") or ""),
        str(body.get("causality_note") or ""),
    ]
    for item in body.get("observations", []):
        if isinstance(item, dict):
            chunks.extend(
                str(item.get(key) or "") for key in ("area", "change", "meaning")
            )
    chunks.extend(str(item) for item in body.get("confounds", []))
    return " ".join(chunks).casefold()


def grade_coaching(report: dict[str, Any], *, radio_available: bool) -> dict[str, Any]:
    body = report.get("report", {})
    text = _report_text(report)
    observations = body.get("observations", [])
    checks = {
        "bounded_report": bool(body.get("summary"))
        and isinstance(observations, list)
        and len(observations) <= 8,
        "natural_corner_language": not any(
            phrase in text
            for phrase in ("started steering", "steering began", "(turn-in)")
        ),
        "corner_specific": "turn " in text or "corner" in text,
        "steering_guidance": any(
            phrase in text
            for phrase in ("steering", "one smooth", "arc", "turn in", "unwind")
        ),
        "speed_control_guidance": any(
            phrase in text
            for phrase in ("throttle", "lift", "coast", "brake")
        ),
        "measurable_checkpoint": bool(re.search(r"\b\d+(?:\.\d+)?\b", text))
        and any(unit in text for unit in ("second", " km/h", "speed", "time")),
        "causality_boundary": any(
            phrase in text
            for phrase in ("association", "associated", "does not prove", "not prove")
        ),
        "missing_radio_honesty": radio_available
        or any(
            phrase in text
            for phrase in (
                "suggestion to test",
                "suggestions to test",
                "transmitter",
                "sanwa",
                "not recorded",
            )
        ),
    }
    passed = sum(bool(value) for value in checks.values())
    return {
        "score": round(100.0 * passed / len(checks), 1),
        "passed": passed == len(checks),
        "checks": checks,
    }


def _run(request_value: dict[str, Any], pipeline_mode: str) -> dict[str, Any]:
    started = time.monotonic()
    response = gateway.call_openclaw(request_value, pipeline_mode=pipeline_mode)
    elapsed_ms = round((time.monotonic() - started) * 1000.0, 1)
    return {"elapsed_ms": elapsed_ms, "response": response}


def _mean(values: list[float]) -> float:
    return round(sum(values) / len(values), 1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--question", default=DEFAULT_QUESTION)
    parser.add_argument("--repetitions", type=int, default=1, choices=range(1, 4))
    parser.add_argument(
        "--database",
        type=Path,
        default=gateway.STATE_DIRECTORY / "companion.sqlite3",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=gateway.STATE_DIRECTORY / "last-single-agent-comparison.json",
    )
    args = parser.parse_args()

    store = CompanionStore(args.database, args.database.parent / "transcript-backups")
    session = store.get_session(args.session_id)
    listed = store.list_messages(args.session_id, 0, limit=2_000)
    history = [
        {"role": item["role"], "content": item["content"]}
        for item in listed["messages"]
        if item["role"] in {"user", "assistant"}
    ][-10:]
    request_value = {
        "mode": "session_chat",
        "contract": "racebox-session-chat-request-v1",
        "question": args.question,
        "session": {
            "context": session["context"],
            "review": session["review"],
            "analytics_csv": "",
        },
        "prior_setup_results": session["prior_results"],
        "history": history,
    }
    radio_available = bool(
        session["review"].get("quality", {}).get("radio_controls_available")
    )

    runs: dict[str, list[dict[str, Any]]] = {
        "adaptive": [],
        "single_agent_test": [],
    }
    for index in range(args.repetitions):
        order = (
            ("adaptive", "single_agent_test")
            if index % 2 == 0
            else ("single_agent_test", "adaptive")
        )
        for pipeline_mode in order:
            print(f"RUN repetition={index + 1} pipeline={pipeline_mode}", flush=True)
            runs[pipeline_mode].append(_run(request_value, pipeline_mode))

    summaries: dict[str, Any] = {}
    for pipeline_mode, samples in runs.items():
        response = samples[-1]["response"]
        token_values = [
            sample["response"].get("model_usage", {}).get("total_tokens")
            for sample in samples
        ]
        known_tokens = [float(value) for value in token_values if value is not None]
        summaries[pipeline_mode] = {
            "pipeline": response.get("pipeline"),
            "route": response.get("route"),
            "model": response.get("model"),
            "thinking": response.get("thinking"),
            "mean_elapsed_ms": _mean([sample["elapsed_ms"] for sample in samples]),
            "elapsed_samples_ms": [sample["elapsed_ms"] for sample in samples],
            "mean_total_tokens": _mean(known_tokens) if known_tokens else None,
            "token_samples": token_values,
            "routing": response.get("routing"),
            "model_usage": response.get("model_usage"),
            "quality": grade_coaching(response, radio_available=radio_available),
            "report": response.get("report"),
        }

    baseline = summaries["adaptive"]
    single = summaries["single_agent_test"]
    token_delta = None
    if baseline["mean_total_tokens"] is not None and single["mean_total_tokens"] is not None:
        token_delta = round(single["mean_total_tokens"] - baseline["mean_total_tokens"], 1)
    quality_not_worse = single["quality"]["score"] >= baseline["quality"]["score"]
    faster = single["mean_elapsed_ms"] < baseline["mean_elapsed_ms"]
    fewer_tokens = token_delta is not None and token_delta < 0
    recommendation = (
        "single_agent_candidate"
        if quality_not_worse and faster and (fewer_tokens or token_delta is None)
        else "keep_adaptive_pending_more_evidence"
    )
    result = {
        "contract": "racebox-single-agent-comparison-v1",
        "created_at": int(time.time()),
        "session_id": args.session_id,
        "session_title": session["title"],
        "source_message_count": listed["total"],
        "question": args.question,
        "raw_telemetry_loaded": False,
        "raw_telemetry_stored": False,
        "comparison": summaries,
        "delta": {
            "single_minus_adaptive_elapsed_ms": round(
                single["mean_elapsed_ms"] - baseline["mean_elapsed_ms"], 1
            ),
            "single_minus_adaptive_total_tokens": token_delta,
            "single_quality_not_worse": quality_not_worse,
            "single_faster": faster,
            "single_fewer_tokens": fewer_tokens if token_delta is not None else None,
        },
        "recommendation": recommendation,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    temporary.write_text(json.dumps(result, indent=2, ensure_ascii=True), encoding="utf-8")
    temporary.replace(args.output)
    print(json.dumps(result, indent=2, ensure_ascii=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
