---
name: racebox-vehicle-dynamics
description: Interpret deterministic RaceBox RC-car previous/current telemetry analytics without overstating causation or sensor capability.
---

# RaceBox Vehicle Dynamics

Use only the calculated analytics and run context supplied by the private
RaceBox gateway. Do not interpret unseen graphs or invent a sensor.

## Decision order

1. Compare the previous run with the current run.
2. Check data quality, complete-lap count, radio coverage, and normal lap spread.
3. Check the top-three lap median. A negative current-minus-previous value is
   faster. Do not call a local response change a gain when the whole-run outcome
   is slower or inside normal variation.
4. Check every recorded confound: ambient/track temperature, tire set, tire runs,
   compound, sauce compound/timing, warmer time/temperature, battery, checklist,
   traffic/damage notes, and driver feel.
5. Treat a single pair as a candidate association. Prefer at least three complete
   laps in each run and a repeated A/B/A or B/A/B test before recommending a setup.

## Calculated indicators

- Lateral response uses absolute chassis lateral G in matched physical
  lap-progress, vehicle-speed, and absolute-steering-input bins. A positive delta means more chassis lateral
  response for similar speed/input. It is not direct tire load or proof of grip.
- Forward bite uses GNSS speed-derived acceleration at at least 90% throttle,
  no meaningful brake, and low steering, matched by physical lap progress,
  speed, and steering bins.
  Battery, gearing, line, surface, and tire state remain alternative causes.
- The brake indicator requires sustained high brake command, a material fall in
  speed-derived deceleration while brake stays high, and reports accompanying
  yaw/lateral/steering disturbance. Call it only a possible lockup or low-grip
  indicator. Confirmed tire lock requires individual wheel-speed data.
- Overdriving/tire-scrub risk means the driver is asking for more steering or
  making more corrections, or the car is bleeding speed without a real brake
  command, while the car does not give matching cornering response. Explain this
  as possible scrub or pushing past the tire window. It is not blame and not
  measured tire temperature.
- Chassis-roll signature first removes stopped-point or straight-line
  asphalt/sensor tilt. Treat it as a roll/loading signature only. It is not
  shock travel, exact roll-center height, tire load, or tire temperature.
- Roll-rate signature uses the same tilt reference, smooths corrected roll
  angle, then measures how quickly that signature builds in deg/sec during
  cornering. Explain it as how fast the car takes a set or loads the outside
  tires. It is uncapped software math, but if the supplied evidence says a
  physical sensor is range-limited, do not trust the clipped peak or claim the
  missing value was recovered.

## Language

Use standard motorsport terms followed by plain meaning. Separate:

- measured change;
- likely driver-action inference;
- the exact input that remains unknown;
- whether the whole-run result improved;
- whether it repeated beyond normal variation;
- the one uncertainty that materially affects the answer.

Never claim direct tire load, suspension load, understeer, oversteer, wheelspin,
or locked wheel unless the supplied measurements actually establish it.

## Driver coaching

- Say what the driver did in natural corner language: "Turn 9: you began turning
  left into the corner 0.160 s earlier than the reference lap." Do not say
  "started steering" or use a raw label such as "(turn-in)" as the explanation.
- Separate the measured result from the proposed technique. A single comparison
  can show that a change was associated with time gained or lost; it cannot prove
  that one input caused the result.
- Honor the requested lap population. If the driver asks about the first eight
  laps, laps before an incident, or a named range, do not substitute every lap.
- Use the full-corner trace: speed and elapsed-time development, longitudinal and
  lateral G, yaw, and sustained path difference. Entry/minimum/average/exit speed
  and one apex offset are summaries, not the whole answer.
- When radio inputs are absent, exact commands are unknown but useful inference is
  still required. Use the supplied driver-action inference to say the pattern is
  most consistent with a lift, coast, brake, combination, or scrub when supported.
  Label it likely/inferred and do not call it a measured transmitter command.
- For "what did I do best?" or "what did I do differently?", explain the driving
  sequence, its time/speed/path consequence, and why it probably worked. Do not
  append a test plan unless the driver asked for one.
- For "how can I go faster?", add one concise steering and one throttle/lift/brake
  instruction only when the evidence supports it. Earlier lift or a short coast is
  not a universal rule; it is useful when the full trace shows the resulting corner
  speed and time were retained.
