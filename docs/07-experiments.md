# 07. Experiments

What to measure, why, and what came out of it. The working log (board states, bench sessions,
raw data, reviews) stays out of the repository, in `lab/`; only results that a conclusion rests on
are written here.

## Planned

| # | What | Answers |
|---|---|---|
| E1 | Two Meshtastic nodes on a private channel, MediumFast, smart position every 20 s; a second run with both as Client Mute (no relays) | real intervals, relay delay, losses; how much "no relays" gains |
| E2 | Range, ShortFast against MediumFast, one node in a vehicle (Meshtastic's Range Test module) | the range table in [02](02-radio-basics.md) |
| E3 | A minimal app that draws two points in OsmAnd over AIDL and moves them every second | whether the `MAP` group is granted without asking, smoothness |
| E4 | How many BLE clients a Meshtastic node accepts; whether a USB client and a BLE client work at once | whether a companion must replace the official app |
| E5 | MeshAnd with two Meshtastic nodes | whether it is a usable baseline; what is missing |

> **Review (Codex, 2026-10-17):** Inference: E1's two radios cannot test the proposed
> 20-rider capacity. Add a controlled 5/10/20-sender load test with known transmitted
> sequence numbers, hidden transmitters, external traffic, relays on/off and a stopped rider.
> Report delivery ratio, median/95th-percentile displayed **fix age**, and help delivery delay;
> received-packet interval or RSSI alone cannot answer the freshness requirement.
>
> Inference: before accepting [06](06-own-protocol.md), also test duplicate slot assignments,
> reboot/late join, loss of PPS, bounded clock drift and two groups together; verify that a
> node stops transmitting once its timing uncertainty exceeds the guard. Exercise route loss,
> reordering, conflicting revisions and transfer while sending help. Check each transmitter's
> complete duty ledger and radiated-power/occupied-bandwidth evidence ([02](02-radio-basics.md)).
> The offline counterexamples in [`review_checks.py`](../tools/review_checks.py) cover arithmetic
> and geometry only; they cannot establish these RF or user-visible outcomes [calc].

## Results

None of E1–E5 has run yet. Measurements the documents already rely on:

- DMD's on-air time: a one-rider DMD packet takes 314 ms on air, measured on dmd2-lora-bridge's
  firmware; `tools/airtime.py` calibrates on it ([04](04-dmd-protocol.md)).
- Route sizes from two real GPX files, computed with `tools/route_size.py`
  ([06](06-own-protocol.md)).

> **Review (Codex, 2026-10-17):** [code] The committed tool's `TOLERANCES=(5, 10, 20, 50)`
> cannot reproduce [06](06-own-protocol.md)'s 2/100/200/500 m rows through its current CLI
> (`tools/route_size.py`, commit `fe4c836018fe91683540dc30c5e24aca1e9ce1cb`). Inference: record
> input SHA-256, tool revision, exact tolerance/quantization and invocation with a published
> result; preserve private GPX privately. The 314 ms calibration is one measured point,
> not independent validation of longer EBYTE frames or their delivery probabilities.
