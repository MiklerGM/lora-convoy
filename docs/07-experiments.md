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

Review (Codex qwen, 2026-10-17): E6 is the right experiment and it is currently
under-specified in four ways that would change its result. [inference]

- **Add a signed-versus-unsigned Meshtastic run.** Signing roughly doubles a position's airtime
  (58 → 106.6 ms, [03](03-alternatives.md)), so a baseline measured unsigned is not the baseline
  [06](06-own-protocol.md) has to beat, which claims authenticated positions. Without this variant
  the comparison is unfair in the wrong direction. [calc]
- **Add the half-duty skip as an observed failure mode.** Positions are gated by channel load,
  while nodeinfo and telemetry stop at **half** the duty allowance
  (`airtime.cpp:319-335`, `airtime.h:232`; not called from `PositionModule.cpp`). On `EU_868`
  that is 5 %. E6 should log "TX air util. >x% Skip send" and the `DUTY_CYCLE_LIMIT` NAKs
  separately, because they are different failures with different thresholds. [code]
- **Add the guard-time measurement** that [06](06-own-protocol.md) needs: per board, the PPS
  alignment error and the RX wake-to-sync latency. The 10 ms guard is currently a guess worth
  about 24 % of SF7/250 capacity. [calc]
- **Give it numeric pass criteria.** "whether the baseline meets the freshness target" has no
  target until Q2 of [01](01-goals.md) is answered, so E6 cannot currently fail. Write the
  criteria as the two headline numbers above (delivered share, 95th-percentile shown-fix age) at
  each of 5, 10 and 20 senders, before it runs — otherwise the result will be read as whatever the
  reader hoped for. [inference]

One caution on the "each radio sends as often as several riders would" trick: it gets the offered
load right, but a real convoy's collisions depend on which riders can hear each other, and a
single radio cannot be in two places. The hidden-transmitter variant is listed, which is good; I
would make it mandatory rather than a variant, since it is the geometry that decides whether
flooding holds a column together. [inference]

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

Review (Codex qwen, 2026-10-17): this is the right test and the only place the repository
commits to measuring rather than modelling. Two additions. It should measure **ERP**, not
transmit-power settings — a configured 22 dBm plus an antenna gain is over the 25 mW ERP cap in
868.0–868.6 MHz, and [02](02-radio-basics.md) already makes the point that a power setting proves
nothing. And it needs a **bandwidth** check against the mode actually used, including the case
[06](06-own-protocol.md) raises of two 125 kHz channels inside one 250 kHz allocation: occupied
bandwidth with oscillator error at both edges is what decides whether that is two channels or one
over-limit emission. [docs]

For the ledger itself, note that Meshtastic's own counters are not enough: its hourly TX share is
60 one-minute buckets of our own transmission (`airtime.h:87,198`), which is close but is a
rolling window rather than a ledger, and it does not attribute airtime to positions versus routes
versus retries. A device that has to prove compliance needs its own per-category counter. [code,
inference]

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
