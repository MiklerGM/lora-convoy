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

## Results

None of E1–E5 has run yet. Measurements the documents already rely on:

- DMD's on-air time: a one-rider DMD packet takes 314 ms on air, measured on dmd2-lora-bridge's
  firmware; `tools/airtime.py` calibrates on it ([04](04-dmd-protocol.md)).
- Route sizes from two real GPX files, computed with `tools/route_size.py`
  ([06](06-own-protocol.md)).
