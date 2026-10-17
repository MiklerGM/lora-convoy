# 07. Experiments

What to measure, why, and what came out of it. The working log (board states, bench sessions,
raw data, reviews) stays out of the repository, in `lab/`; only results that a conclusion rests on
are written here.

**What counts as a result.** The requirement in [01](01-goals.md) is about the rider's map. The
headline numbers are therefore:

- the share of known, transmitted sequence numbers that arrive;
- the median and 95th-percentile age of the fix each map shows;
- how long a help message takes to arrive, and to be acknowledged.

Packet intervals or RSSI alone cannot answer it.

## Planned: the baseline

| # | What | Answers |
|---|---|---|
| E1 | Two Meshtastic nodes on a private channel, MediumFast, smart position every 20 s; a second run with both as Client Mute (no relays) | real intervals, relay delay, losses; how much "no relays" gains |
| E2 | Range, ShortFast against MediumFast, one node in a vehicle (Meshtastic's Range Test module) | the range table in [02](02-radio-basics.md) |
| E3 | A minimal app that draws two points in OsmAnd over AIDL and moves them every second | what the rider must do before points appear (a new app starts disabled, [05](05-osmand-integration.md)); smoothness |
| E4 | How many BLE clients a Meshtastic node accepts; whether a USB client and a BLE client work at once | whether a companion must replace the official app |
| E5 | MeshAnd with two Meshtastic nodes | whether it is a usable baseline; what is missing |
| E6 | Load: 5, 10 and 20 senders on one channel, each logging the sequence numbers it sends. Variants: relays on and off; hidden transmitters (senders out of each other's range); other traffic on the channel; one rider stopped, so smart position sends less | whether the baseline meets the freshness target, and at what group size it stops; the numbers [06](06-own-protocol.md) has to beat |

E1's two radios cannot show what twenty riders do; E6 can. With fewer radios than riders, each
radio can send as often as several riders would. The channel load is then right but the geometry
is not, so hidden transmitters still need real separation.

## Before accepting 06

[06](06-own-protocol.md) is a set of notes. An implementation of it gets E6's measurements on the
same terms, and these tests besides:

- **Slots and roster:** two radios given the same slot; a radio that reboots or joins late listens
  before it transmits; a roster change takes effect at its activation epoch.
- **Timing:** loss of PPS, and of the GPS fix; drift held within bounds; a radio stops
  transmitting once its timing uncertainty exceeds the guard.
- **Neighbours:** two groups on the same frequency.
- **Routes:** route packets lost or reordered, conflicting revisions, and a help message sent
  during a transfer.
- **Regulation:** each transmitter's complete duty ledger over an hour (positions, relays, routes,
  retries, control), and measured radiated power and occupied bandwidth against the sub-band's
  limits ([02](02-radio-basics.md)).

`tools/cross_check.py` checks the arithmetic and the route geometry offline, against an independent
derivation and known counterexamples. None of the tests above can be settled without radios.

## Results

None of E1–E6 has run yet. Measurements the documents already rely on:

- **DMD's time on air:** a one-rider DMD packet (46 bytes) takes 314 ms, measured on
  dmd2-lora-bridge's firmware. `tools/airtime.py` calibrates DMD's fixed overhead on it
  ([04](04-dmd-protocol.md)). It is a single point: longer packets are extrapolated from it, and it
  says nothing about delivery.
- **DMD's delivery on two rides**, from dmd2-lora-bridge's logs: about 45 % of the other device's
  packets on 2026-09-29 and 49 % on 10-02, on days with other known losses
  ([04](04-dmd-protocol.md)).
- **Route sizes** ([06](06-own-protocol.md)): `python3 -I tools/route_size.py FILE...` with its
  default tolerances, 2 to 500 m. That is a 1e-5° grid, 230 bytes of route per packet and a 24-byte
  manifest per route. The numbers are from the tool as committed with this document; whoever changes
  the tool re-runs it and updates them.

  | Input | SHA-256 |
  |---|---|
  | the owner's multi-day route, kept private | `24d1aa6b681502b5d0fe7b7a072ffa4271fd239b2c3b02d6bb01ff499bad7524` |
  | Trans-Morocco Trail map v3.9, public | `aac93acfc73bd47caf316049c19b30188b478e7be9c3fb58fa28d25b34fc7b53` |
