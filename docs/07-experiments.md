# 07. Experiments

What to measure, why, and what came out of it. The working log (board states, bench sessions,
raw data, reviews) stays out of the repository, in `lab/`; only results that a conclusion rests on
are written here.

**What counts as a result.** The requirement in [01](01-goals.md) is about the rider's map. The
headline numbers are therefore:

- the share of known, transmitted sequence numbers that arrive;
- the median and 95th-percentile age of the fix each map shows;
- how long a rider's state, "need help" first, takes from its choice to its showing in another
  rider's app.

Packet intervals or RSSI alone cannot answer it. Logs carry millisecond timestamps referenced to
GPS time at both ends: Meshtastic's own fix times are whole seconds [code: `mesh.pb.h`,
`timestamp_millis_adjust` unset at `fad52bb`], too coarse for a target of 1–2 s.

## Planned: the baseline

| # | What | Answers |
|---|---|---|
| E0 | Timing on each board: whether it has a PPS line; the PPS edge against the radio's start of transmission; without PPS, the jitter of NMEA time; the jitter of starting a transmission | whether the schedule in [06](06-own-protocol.md) can run on that board, and the terms of its guard |
| E1 | Two Meshtastic nodes on a private channel, on MediumFast, or ShortFast where the public mesh is on MediumFast; smart position every 20 s; a second run with both as Client Mute (no relays) | real intervals, relay delay, losses; how much "no relays" gains |
| E2 | Range: ShortFast and MediumFast, and SF9/125 and SF10/125 through Meshtastic's custom modem settings, the modes [06](06-own-protocol.md) picks; one node in a vehicle (Meshtastic's Range Test module) | the range tables in [02](02-radio-basics.md), per sub-band |
| E3 | A minimal app that draws two points in OsmAnd over AIDL and moves them every second | what the rider must do before points appear (a new app starts disabled, [05](05-osmand-integration.md)); smoothness |
| E4 | How many BLE clients a Meshtastic node accepts; whether a USB client and a BLE client work at once | whether a companion must replace the official app |
| E5 | MeshAnd with two Meshtastic nodes | whether it is a usable baseline; what is missing |
| E6 | Load in Meshtastic's simulator: 5, 10 and 20 nodes on the stock firmware, each logging the sequence numbers it sends, with hidden nodes (out of each other's range) in every run, checked against the same scenario on three real radios. Variants: authentication none, AEAD, signed, or both; relays on and off; a second group on the channel, with rebroadcast mode `ALL` or `LOCAL_ONLY`; one rider stopped, so smart position sends less | whether the baseline meets the freshness target, and at what group size it stops; the numbers [06](06-own-protocol.md) has to beat |
| E7 | Soak: three hours or more with the phone's screen off and OsmAnd navigating, node and companion running | gaps from the phone and the node: Android putting the app to sleep, Bluetooth drops, a node that hangs; counted as stretches without a shown update |
| E8 | Ride replay: 1 Hz phone GPS logs of every rider on one real ride, with every radio's receive log | how far the column spreads (question 3 in [01](01-goals.md)); how often a sender would send if it sent only when the others' prediction drifts; the hearing graph for relay selection ([06](06-own-protocol.md)) |
| E9 | The requirement: riders watch one ride replayed at 2 s and at 10 s with marked prediction, and judge stops and turns | whether 10–15 s with prediction is enough (question 2) |
| E10 | Channel survey: occupancy of 869.40–869.65 MHz along a route, and in a city for contrast, with the RTL-SDR | how much of Meshtastic's 25 % gate, and of a schedule's frame, other users take |
| E11 | Height: the same links with a relay antenna at 1.2, 3 and 7 m, in hills | whether a raised relay gains the 8–15 dB the two-ray model predicts |
| E12 | A DMD rider nearby: an EBYTE module with DMD's profile (868.125 MHz, 500 kHz, its pacing and power) 0.5–5 m from a node that receives a weak signal from a distant one | whether a DMD device in a mixed group deafens our radios, and from what distance. The modules are already on hand ([04](04-dmd-protocol.md)); runs stay short, since DMD's configured power and pacing are over that sub-band's limits (for information) |

**Where to run them.** A city is the worst case for neighbours, and the trail is the target. E1
and E2 run out of town, or record the public load as a condition of the run; E10 measures both
places.

**E6 in detail:**

- **Where it runs.** Meshtastic's own simulator,
  [Meshtasticator](https://github.com/meshtastic/Meshtasticator), runs one instance of the stock
  firmware's Linux build per node and hands each packet to the nodes that would hear it, by their
  positions and a path-loss model [docs: its `INTERACTIVE_SIM.md`]. All logs are on one clock, so
  shown-fix ages need no GPS. Its limits:
  - collisions are emulated only with a build flag, and then any overlap loses both packets, with
    no capture effect [code: `SimRadio.cpp:306-341` at `fad52bb`], so losses err high;
  - its path-loss model is rough;
  - riders do not move, so positions go out on a fixed interval, and the stopped-rider variant
    runs on the real radios.
- **Calibration on air.** The same scenario runs on three radios, two in range of the third but
  not of each other. The simulator's figures for 10 and 20 nodes count only if, for those three,
  its delivered share, channel load and gate skips match the radios'.
- **Pass criteria are written before it runs:** the delivered share and the 95th-percentile
  shown-fix age at 5, 10 and 20 senders, against the same measures of our candidate setups in the
  same scenario ([09](09-protocol-next.md)), since question 2 in [01](01-goals.md) sets no number.
  Without them, E6 cannot fail.
- **The like-for-like arm is AEAD**, about 10.5 s for 20 riders on paper: the protocol in
  [06](06-own-protocol.md) proves group membership the same way. Signing proves which rider sent a
  packet, a different guarantee ([03](03-alternatives.md)).
- **Separate log lines for separate failures:** "Ch. util" skips stop positions, "TX air util"
  skips stop nodeinfo and telemetry at half the duty allowance, and `DUTY_CYCLE_LIMIT` stops
  everything ([02](02-radio-basics.md)). Each node's hourly transmit share is logged next to its
  gate skips, to tell which limit binds; repeated `(from, id)` pairs are logged too
  ([03](03-alternatives.md)).
- **On stock firmware one radio cannot play several riders:** positions run on a 5 s tick, a
  client cannot set the sender, so stand-ins share one node number, and the per-node gates see the
  sum [code: `PositionModule.cpp:503,578`, `MeshService.cpp:312` at `fad52bb`]. A stand-in run
  measures channel load only, which is why the load runs in the simulator.

E1's two radios cannot show what twenty riders do; E6 can, in the simulator.

**E7 is not hypothetical.** On our own rides Samsung's "sleeping apps" list overrode the battery
exemption, and a bridge hung once until a power cycle [bench: dmd2-lora-bridge
`docs/ROADMAP.md`, `docs/ARCHITECTURE.md`].

## Before accepting 06

[06](06-own-protocol.md) is a set of notes. An implementation of it gets E6's measurements on
comparable terms, E0 on every board it supports, and these tests besides. Its slots never overlap
by design, so on the bench one radio may play several riders, through a dummy load since it then
carries their airtime too; independent clocks take the real radios.

- **Slots and roster:** two radios given the same slot, which the design must prevent since their
  nonces would repeat; a radio that reboots without a fix, or joins late, listens before it
  transmits and never reuses a session; a roster change takes effect at its activation epoch; the
  organiser's radio is lost mid-ride.
- **Timing:** loss of PPS, and of the GPS fix; drift held within bounds; a radio stops
  transmitting once its timing uncertainty exceeds the guard. The simulator does all three (M6),
  and a board reports what E0 needs in its status: `lag=` on a Heltec, its NMEA against its PPS,
  and on every board `err=`, `late=` and `unsure=` ([PROTOCOL](PROTOCOL.md), the phone API).
- **Neighbours:** two groups on the same frequency.
- **Routes:** route packets lost or reordered, conflicting revisions, and a help message sent
  during a transfer.
- **Regulation:**
  - each transmitter's complete duty ledger over an hour, by category: positions, relays, routes,
    retries, control. Meshtastic's counters are rolling windows with no categories, so the device
    keeps its own;
  - the radiated power measured as ERP, not taken from the power setting;
  - the occupied bandwidth, with oscillator error at both edges. It decides whether two 125 kHz
    channels fit one 250 kHz allocation ([06](06-own-protocol.md)).

`tools/cross_check.py` checks the arithmetic and the route geometry offline, against an independent
derivation and known counterexamples. None of the tests above can be settled without radios.

## Results

### E0 and the schedule on two boards, 2026-11-11

Stages 0–3 of [13](13-test-plan.md), indoors on one desk: a Heltec V4.3 with its GNSS kit (PPS) and a
Wio Tracker L1 Pro (no PPS), an RTL-SDR without antenna as the third witness. Firmware 4ec578f, from
15:32 with a bench change, since in every build: a `GNSS 0|1` console command and the
L76K told to send GGA and RMC only (`$PCAS03`). Working log and per-run numbers: `lab/2026-11-11-notes.md`.

- **The SDR and the Heltec agree.** The L1's lateness by the Heltec's `offset=` less the Heltec's own
  `txlead=`, against the SDR's burst starts, in five 5-minute windows: within 0.2 ms each time. From
  then on the Heltec is the reference.
- **NMEA lag grows with the satellites while the L76K sends its default sentences.** RMC comes last,
  after GSA and GSV, at 9600 baud: from the Heltec's PPS 520 ms at 6 satellites, ~700 at 12, 871 at
  most, near the 900 ms pairing limit; the L1 ~90 ms late with no fix, ~190 ms at 4–5 satellites.
  That is the plan's stop condition: a lateness that wanders by more than 100 ms.
- **With GGA and RMC only** the lag is 222–322 ms at 11–12 satellites (median 234, p95 278); the L1
  was 187–280 ms late over 30 minutes, sd 14, 196 at 5–6 satellites and 226 at 10–12. A spread of
  some ±40 ms from second to second remains, the receiver's own.
- **The L1 told its lag** (217 ms, ±50 ms): mean 8.5 ms late, of which its own start of transmission
  is 7.1; p1 −7, p99 +27, most +54 (n=298). Over the next two hours on air, in groups of 10 and 13
  (3,503 packets), the earliest and latest start the Heltec recorded for it were −26.3 and +59.1 ms:
  one packet beyond what its claim allows (57 ms: its 50 and its 7 ms start), by 2 ms.
- **Start of transmission:** 7.9–8.1 ms on the Heltec, 7.1–7.3 ms on the L1, steady to 0.2 ms; the
  schedule allows 1 ms (`TX_ERR_US`). SetStandby(RC) stops the TCXO before each packet and SetTx waits
  its 5 ms start, plus the Heltec's 1 ms front end.
- **Drift without GNSS** (the L1's L76K on standby for an hour): the L1's clock 29.4 ppm fast, a
  straight line (residual sd 0.9 ms); its `err=` grew at the 50 ppm bound, 50 to 229 ms, the true
  error to ~99 ms. Indoors, steady temperature.
- **A packet** is 31.06 ms (Heltec) and 31.10 ms (L1) at half power; computed 30.85.
- **The schedule** ([13](13-test-plan.md) stage 3, the L1 at ±50 ms, 2 s frames): neighbours in a group
  of 10 for an hour, every packet taken both ways (1719/1719, 1711/1711), never closer than 126 ms on
  air; 13 is the largest group the L1 joins (budget 55 ms) and held for an hour with no overlap; in 14,
  20 and 32 it stays silent, counting `unsure=`, as in SF9 in a group of 10 (budget 38 ms). Both in one
  slot: the first `clash=` after 21 s. Two groups on the channel: overlaps in 4 % of frames, scattered,
  the other's packets counted `foreign=` and never shown as riders. A restart: heard again in 12 s, a
  new session. `LEDGER`: 53.4 and 53.9 s in an hour, as computed. A lag told 181 ms wrong puts the
  L1 on its neighbour's slot every frame, and nothing flags it (`clash=0`); 100 ms wrong at 182 ms
  spacing does not reach the neighbour, contrary to the plan's expectation.

- **With both in the firmware** (the same evening, 30 minutes, neighbours in a group of 10): the
  L76K told GGA and RMC only in every build, the L1 built with its lag (210 ms, ±50; the 217 above
  held its 7 ms of start) and each board handing its packet over its radio's start-up early.
  `txlead=` fell to −0.4/−0.1/0.0 ms on the Heltec and 0.0/0.0/0.2 ms on the L1. The L1 at the
  Heltec: −10.8 to +62.1 ms, mean 8.0, sd 8.5, p99 34 (880/880 taken; 865/865 the other way); the
  SDR over five of those minutes agrees within 0.4 ms (1.9 against 2.2), bursts never closer than
  139 ms. The sky had changed: 14 satellites at the Heltec and its own lag 256 ms, against 234 in
  the afternoon. One packet in 880 came later than the L1's ±50 ms allows, by 12 ms.

Not covered: range (E2), battery, the phone, a power cycle (only software restarts), stage 3.9 as
written (a key and time but no fix was seen silent instead), outdoor sky and temperature.

### Earlier

Measurements the documents already rely on:

- **DMD's time on air:** a one-rider DMD packet (46 bytes) takes 314 ms, measured on
  dmd2-lora-bridge's firmware. `tools/airtime.py` calibrates DMD's fixed overhead on it
  ([04](04-dmd-protocol.md)). It is a single point: longer packets are extrapolated from it, and it
  says nothing about delivery.
- **DMD's delivery on two rides**, from dmd2-lora-bridge's logs: about 45 % of the other device's
  packets on 2026-09-29, and 49 % between the tablet and the NOR7E on 10-02, with other known losses
  ([04](04-dmd-protocol.md)).
- **Route sizes** ([06](06-own-protocol.md)): `python3 -I tools/route_size.py FILE...` with its
  default tolerances, 2 to 500 m. That is a 1e-5° grid, 230 bytes of route per packet and a 24-byte
  manifest per route. The numbers are from the tool as committed with this document; whoever changes
  the tool re-runs it and updates them.

  | Input | SHA-256 |
  |---|---|
  | the owner's multi-day route, kept private | `24d1aa6b681502b5d0fe7b7a072ffa4271fd239b2c3b02d6bb01ff499bad7524` |
  | Trans-Morocco Trail map v3.9, public | `aac93acfc73bd47caf316049c19b30188b478e7be9c3fb58fa28d25b34fc7b53` |
