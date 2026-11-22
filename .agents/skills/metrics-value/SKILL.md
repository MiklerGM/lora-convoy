---
name: metrics-value
description: Review how firmware counters, simulator results, and bench or field logs answer questions about delivery, freshness, airtime, and availability in LoRa Convoy.
---

# Metrics value

Start with the decision the measurement should support. For example, to decide whether the last rider can follow a turn, measure how long that rider has a usable position for the leader. A packet count alone cannot answer that question. The [ride requirements](../../../docs/01-goals.md) name the outcomes the group cares about.

Trace one sample from its producer through the decoder or evaluator to the reported value. Choose the checks that affect its meaning.

- **Age and availability.** Identify the source timestamp, its clock, and the event that refreshes it. A heartbeat or a relayed copy can refresh contact while the fix keeps ageing. For freshness over a ride, weight by elapsed time and report the time without a known position. A percentile sampled only on arrival omits the silence between packets.
- **Counts and ratios.** Name the receiver, direction, observation window, numerator, and denominator. Check what a wrap, reboot, or gap does to the arithmetic. If a rolling counter permits several totals, report the ambiguity or compare with the sender's log. For airtime, say whether the board reserved the time, requested TX, or a radio measurement observed the burst.
- **Comparisons.** Record the effective profile and the conditions that could change the decision. For two simulated protocols, use the same movement, outages, and seed. When comparing a simulation to a ride, check the actual heartbeat, relay settings, and active radios. The [simulator guide](../../../tools/simulator/README.md) documents which parts run the C core and which are modeled.

Verify the signal at the cheapest boundary that produces it. Use a counter across a known gap, a replay with a known missing fix, or a short slice of a field log as appropriate. Report the resulting value with its denominator and evidence source. Keep, revise, or drop a signal according to the decision it can support.
