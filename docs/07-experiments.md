# 07. Experiments

What to measure, why, and what came out of it. The working log (board states, bench sessions,
raw data, reviews) stays out of the repository, in `lab/`; only results that a conclusion rests on
are written here.

**What counts as a result.** The requirement in [01](01-goals.md) is about the rider's map. The
headline numbers are therefore:

- the share of known, transmitted sequence numbers that arrive;
- the median and 95th-percentile age of the fix each map shows;
- how long a help message takes to arrive, and to be acknowledged.

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
  shown-fix age at 5, 10 and 20 senders, against the target set by question 2 in [01](01-goals.md).
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
  transmitting once its timing uncertainty exceeds the guard.
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

None of E0–E12 has run yet. Measurements the documents already rely on:

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
