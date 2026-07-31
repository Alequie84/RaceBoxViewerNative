# Driver Analysis v2 Evidence Engine

Status: integrated into `driver-analysis-v2`, native insight cards, workspace persistence, and JSON export.

## Purpose

This engine prevents a favorable-looking telemetry value from becoming a recommendation unless it also produces a meaningful time gain, retains that gain to the next driver decision, repeats on quality-screened matching laps, and passes available data and driving guardrails.

The classifier remains independent of ImGui so it is deterministic and directly testable. Its public contract is `include/racebox/insight_evidence.hpp`; `src/core/insight_evidence.cpp` implements classification, and `src/core/driver_analysis.cpp` supplies real per-phase and repeated-lap evidence.

## Three independent answers

The add-on never compresses all evidence into one `positive` Boolean. It returns:

1. **Outcome**: data limited, inconclusive, net loss, compensation, retained gain, or trade-off gain.
2. **Reliability**: unproven, likely, or reliable session association.
3. **Recommendation**: none, observation, validate, recommend technique, or recommend the complete sequence.

This separation allows a measured gain to remain reliable evidence while a track-limit violation or instability indicator blocks the recommendation.

## Sign and timing contract

Timing effects match the relative-time graph:

- positive = time lost;
- negative = time gained.

The dynamic timing noise floor is:

```text
max(0.05 s, 2 x median sample period, 1.5 x measured timing-repeatability sigma)
```

The integrated phase extractor samples cumulative position-based Delta-T at the corner start, the candidate action, corner end, and the next configured corner start (or lap end). It supplies time already lost before the action, local response through corner end, and retained result at the next driver decision.

## Recommendation gates

A normal technique recommendation requires all of the following:

- retained gain beyond the dynamic noise floor;
- data confidence of at least 75;
- at least eight quality-screened matching laps;
- at least five supporting laps;
- at least 75 percent support;
- an aggregate median beyond the noise floor;
- the upper end of the reliability interval beyond the noise floor;
- no track-limit violation, extra steering correction, or possible-instability guardrail;
- no more than 50 percent downstream payback.

A slower-entry/faster-exit pattern can only recommend the complete sequence. A better local metric followed by a net loss is always compensation and never a recommendation. One selected lap is always an observation, never reliable evidence.

## Verified counterexamples

`racebox_insight_evidence_addon` covers both classifier counterexamples and live-pipeline integration:

- higher exit speed after an unrecovered entry loss;
- local recovery that only returns to timing noise;
- straightforward net time loss;
- one-lap gain without repeatability;
- repeated retained gain;
- a genuine slower-entry/faster-exit trade-off;
- more than 50 percent downstream payback;
- likely evidence whose interval still crosses noise;
- low support rate;
- track limits, steering correction, low confidence, context changes, GPS-line disablement, and invalid inputs;
- a golden-session harness using R15 as reference and R6/R8 as comparisons;
- real session aggregation, dynamic noise floor, support counts, and recommendation gates on every visible card.

The current golden harness reviews 107 integrated cards. It finds six recovery/compensation cases and 24 retained/trade-off observations; zero currently clear the reliable recommendation gate. The test asserts that compensation cannot recommend and that any future recommendation must be backed by reliable evidence and the configured lap/support minima.

## Current integration

The native app now provides:

- per-corner cumulative Delta-T checkpoints through the next driver decision;
- quality-screened matching-lap classification;
- robust median, support rate, repeatability sigma, and interval calculation across the session;
- archive fields for formula-v2 evidence and rules;
- cards that expose outcome, reliability, recommendation, timing effects, noise floor, support, confidence, and reasons;
- editable evidence gates with reset, archive restore, and analysis-JSON export.

The matching-lap screen currently requires a complete lap, no telemetry gap, sufficient confidence, the same triggered metric direction, and line metrics within the GPS-correction gate when applicable. The source recordings have no explicit track-limit channel or structured setup/conditions-change markers, so those guardrails cannot yet be populated automatically. Steering-correction guardrails are populated from detected events. Any future source field for track limits, yellow flags, traffic, damage, tires, setup, or weather should feed the existing `CandidateEvidence` guardrails rather than changing the classifier's outcome definitions.
