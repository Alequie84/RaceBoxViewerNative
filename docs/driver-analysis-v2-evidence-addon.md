# Driver Analysis v2 Evidence Add-on

Status: isolated pre-integration module. The native UI still uses `driver-analysis-v1`.

## Purpose

This add-on prevents a favorable-looking telemetry value from becoming a recommendation unless it also produces a meaningful time gain, retains that gain to the next driver decision, repeats on comparable clean laps, and passes data and driving guardrails.

The implementation is deliberately independent of ImGui, session persistence, and the current insight-card generator. Its public contract is `include/racebox/insight_evidence.hpp`; its deterministic classifier is `src/core/insight_evidence.cpp`.

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

The add-on receives the time already lost before the candidate action, the local time response, and the retained result at the next braking point or corner start. The future v2 phase extractor will calculate those values from cumulative position-based Delta-T.

## Recommendation gates

A normal technique recommendation requires all of the following:

- retained gain beyond the dynamic noise floor;
- data confidence of at least 75;
- at least eight comparable clean laps;
- at least five supporting laps;
- at least 75 percent support;
- an aggregate median beyond the noise floor;
- the upper end of the reliability interval beyond the noise floor;
- no track-limit violation, extra steering correction, or possible-instability guardrail;
- no more than 50 percent downstream payback.

A slower-entry/faster-exit pattern can only recommend the complete sequence. A better local metric followed by a net loss is always compensation and never a recommendation. One selected lap is always an observation, never reliable evidence.

## Verified counterexamples

`racebox_insight_evidence_addon` covers:

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
- a golden-session harness using R15 as reference and R6/R8 as comparisons.

The golden harness intentionally provides no invented multi-lap aggregate. It verifies that every current v1 card remains an observation, loss, compensation, or inconclusive result and that none becomes a recommendation before the repeated-lap extractor exists.

## Integration hold point

Do not connect the add-on to the visible Insights panel until a second implementation stage provides:

- per-corner cumulative Delta-T checkpoints through the next driver decision;
- clean/comparable-lap classification;
- robust median, support rate, repeatability sigma, and interval calculation across the session;
- archive fields for formula-v2 evidence and rules;
- wording and UI tests for every outcome/reliability/recommendation combination.
