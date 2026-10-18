# 06. A protocol of our own: design notes

**Status:** notes, not a specification. Nothing here is implemented or measured; every capacity
figure is [calc] and is an **offered** budget, not delivered freshness.

## When it is worth building

Tuned Meshtastic with an OsmAnd companion ([03](03-alternatives.md), [05](05-osmand-integration.md))
is the baseline, along with MeshCore-Solo. A protocol of our own pays off only against a measured
shortfall of that baseline ([07](07-experiments.md)):

- more than about 10 riders who each need a position every 5 s or more often;
- positions more often than every 5 s for anyone;
- long, stretched columns where flooding does not hold the group together.

The last one is **not** solved by v1 below, which has no relay at all; faster direct broadcasts do
not cross a ridge. It can only be compared once v2's forwarding exists. If none of these applies,
building our own means paying for firmware on every board, field testing, and losing
interoperability with plain Meshtastic users, for no gain.

## What to avoid: "DMD, but better"

DMD's core is random access plus every device retelling every rider, at a fast pace
([04](04-dmd-protocol.md)), and its channel is over capacity at six riders. Smaller packets or real
encryption on top of that design would not change this, so a better DMD is the wrong target.

## Principles

1. **Scheduled access.** Each rider owns a time slot in a repeating frame (TDMA), timed by GPS. No
   collisions inside the group, including between riders who cannot hear each other, and the
   channel can be planned to 70–80 % instead of the 25 % random access tolerates. That figure is a
   planning assumption: it needs bounded clock error, unique slots and known packet lengths, and
   other users of the band still interfere.
2. **Each rider sends only their own position**, in a small binary packet.
3. **Relaying is separate and selective**: dedicated slots, only where needed (v2).
4. **One clock: GPS time.** A position carries its fix time; age is now minus that. A relayed
   position never gets younger.
5. **Cadence adapts**: faster when moving and close together, slower when stopped.
6. **Authenticated encryption** with a group key, with nonces that never repeat (below).
7. **One duty budget per device**: positions, relays, routes, retries and control together stay
   inside the sub-band's limits for duty cycle and ERP.

## Capacity [calc]

Group capacity in positions per second; 24-byte packet, CR 4/5, explicit header, CRC, 8-symbol
preamble, 10 ms guard, 80 % of the frame used for slots. Derived independently in
`tools/cross_check.py`:

| Approach | Time on air | Positions/s for the group | 10 riders | 20 riders |
|---|---|---|---|---|
| DMD, stock | 314–990 ms | over capacity at six riders | — | — |
| Meshtastic ShortFast, one relay, 25 % load | 58 ms | 2.2 | every ~4.6 s | every ~9.3 s |
| Meshtastic MediumFast, one relay | 191 ms | 0.7 | every ~15 s | every ~31 s |
| Slots by GPS time, SF7/250 | 30.848 ms | 19.6 | every 0.51 s | every 1.021 s |
| Slots by GPS time, SF8/250 | 56.576 ms | 12.0 | every 0.83 s | every 1.664 s |
| Slots by GPS time, SF9/250 (range like MediumFast) | 102.912 ms | 7.1 | every 1.41 s | every 2.823 s |

One rider sending every second is on air 3.1 % of the time at SF7, 5.7 % at SF8 and 10.3 % at SF9.
At SF9 that is **over** a 10 % budget before any other traffic.

## v1: up to 20 riders, no relaying

The simplest version that could work.

### The roster and the slots

The organiser creates a group QR code once. It holds:

- the group key;
- radio parameters (frequency, SF, bandwidth);
- the frame length;
- the roster: each rider's slot.

Rules:

- **Each physical radio takes exactly one roster entry**, and two radios never share a slot under
  one key: their nonces would repeat (security rules below). The roster carries a version and an
  activation epoch, both authenticated. A changed roster or frame takes effect at an agreed future
  epoch, or old and new schedules overlap.
- **Spare slots** (about one per ten riders) have a single allocator, the organiser's radio.
  Otherwise two late joiners who cannot hear each other pick the same "free" slot. If that radio is
  lost, nobody hands out slots any more; the QR could name a deputy (open questions).
- **A radio that restarts or joins late listens for a frame before it transmits.**

### Timing

- **Frames are numbered from an epoch:** frame k starts at `epoch + k × frame_length`, in GPS time,
  with GPS against UTC defined. A 1.2 s frame cannot restart on every whole second.
- **The time reference is the GPS's PPS pin, not its serial messages,** which arrive after the fix
  is computed [docs].
- **Not every board has a PPS line.** In Meshtastic's pin maps only the Heltec V4 (with its GNSS
  expansion) and the RAK4631 (with the RAK1910) wire one. The Wio Tracker L1 connects its GNSS by
  UART only, the Heltec V3 has no GNSS, and the T-Deck's GPS is optional [code: `variants/` at
  `fad52bb`; the L1 schematic is not checked]. Without PPS the time comes from NMEA sentences, which
  at 9600 baud take about 70 ms to clock out [calc]. Each board's timing is measured before the
  schedule is sized ([07](07-experiments.md), E0).
- **Without a fix,** a radio recovers frame phase from authenticated, replay-checked packets it
  hears: their slot is known, and the packet's own duration is taken into account. That is only as
  precise as the symbol time: 0.5 ms at SF7/250, 8 ms at SF10/125. The phone's clock over BLE is a
  last resort, and only with a known error bound.
- **The guard is a sum of named terms**, not a round number: the time source's error, the
  transmitter's start jitter, propagation (33 µs at 10 km), and the drift allowed between
  resynchronisations. Drift over one frame is tiny (two ±20 ppm clocks part by 48 µs in 1.2 s), so
  the guard is mostly a choice of holdover [calc]:
  - a 10 ms guard lasts 250 s without PPS, a 2 ms guard 50 s; a clock disciplined against PPS might
    hold about 1 ppm and stretch both (not measured);
  - a 2 ms guard instead of 10 ms gives 24 % more positions per second at SF7/250, 8 % at SF9/250
    and 2 % at SF10/125.

  A radio transmits only while its timing uncertainty fits inside the guard, and goes quiet
  otherwise. In v1 every node listens all the time, so receiver wake-up is not a term.
- **Other groups can still collide** with us; see the open questions.

### Range

Out of range means stale, not lost: a rider beyond direct range keeps their last point on
everyone's map, with a growing age, until they come back. Range is direct radio range: around 1 km
in forest (a guess). On an open road it depends on the sub-band as much as on the mode: with
dipoles at 22 dBm in 869.40–869.65 MHz, 2.7–6 km at SF7/250 and 3.5–7.9 km at SF9/250, and less in
868.0–868.6 MHz ([02](02-radio-basics.md)).

### Packet sketch, about 24 bytes

The fields below sum to 17 bytes plus a 4–8 byte tag, so 21–25 bytes, and the session in the
nonce has no field yet (security rules below). Two properties of LoRa shape the final length
[calc; preamble 8, CR 4/5]:

- **Time on air grows in steps.** With an explicit header and CRC, 23–26 bytes cost the same as 24
  at SF7/250 and SF8/250, 22–26 at SF9/250. At SF10/125, 24 bytes sits at the top of its step: 25
  bytes take 11 % longer. The length, tag and session included, is fixed before any capacity
  figure is final, and chosen just under a step.
- **The PHY CRC is redundant under a MAC, and a fixed length allows an implicit header.** At 24
  bytes, dropping either one saves 8–9 % at SF7/250 and SF8/250; at SF9/250 only the implicit
  header saves (10 %); at SF10/125 only both together do (11 %). Without the CRC, 22–26 bytes cost
  the same as 24 at SF10/125, which leaves room for a session field. An implicit header also means
  every node must know every packet length in advance, which makes the typed slots below harder.

| Bytes | Field |
|---|---|
| 1 | version (2 bits), type (2 bits), flags (4 bits) |
| 1 | slot / rider index |
| 3 | transmission counter, per sender, within a session that starts at a reboot-safe epoch |
| 2 | GPS time of the fix, seconds modulo 65 536, unwrapped within ±9 h |
| 3 + 3 | latitude, longitude, 24 bits each |
| 1 | speed, km/h |
| 1 | course, 2° steps |
| 1 | status: help, stopped, GPS ok, battery low |
| 1 | route version and "have it all" bit, for route transfer (below) |
| 4–8 | message authentication code (AES-CCM); its length is still to be budgeted |

**Speed and course stay.** Differenced from two fixes 1 s apart, 2.4 m coordinate steps make the
speed err by up to ±8.6 km/h, a lost packet leaves nothing to difference, and a stale value is
already flagged by the fix time and the "GPS ok" bit [calc]. At SF9/250 and SF10/125 the two bytes
cost no airtime at all.

**The fix time has whole-second resolution**, coarse for a cadence of 1–2 s: a receiver cannot
tell a 0.2 s old fix from a 1.1 s old one. The slot already gives the time of transmission, so the
fix's age at that moment, in fractions of a second, would be finer and could be shorter (open
questions).

**Security rules** [docs]:

- **A nonce never repeats under one key** (NIST SP 800-38C, §5.3), so it is never built from the
  fix time, which repeats when the GPS stalls and wraps after 18 h 12 min. Two constructions work:
  - group, slot, session and counter, where the session is random (32 bits or more) or kept in
    flash, so a reboot without a fix never reuses one; or
  - group, slot and the frame number k from GPS time, as TSCH does with its absolute slot number.
    It needs no session field, a rebooted radio learns k from any packet it hears, and it stays
    unique while each slot sends once per frame.
- **The header is authenticated, and replay is checked per slot.** With the frame number, a
  receiver drops any packet whose k is more than a few frames from its own clock. With counters it
  keeps the highest counter per slot, and after its own reboot it cannot tell a replayed packet
  from a fresh one until it has heard that sender again.
- **A 32-bit tag needs a forgery-risk and key-lifetime budget**; NIST (Appendix B) requires that
  analysis below 64 bits. Settle it before fixing the packet length.
- **A group key authenticates membership, not a rider**: any member can build another slot's
  packet (RFC 3610, §2.1). Route and control messages are authenticated too.
- **There is no key rotation during a ride.** The key travels only in the QR, and a new key sent
  under the old one reaches a lost radio too; until the group meets, a lost radio can read and
  forge positions. Keys change when a device is lost or a rider leaves, at the next regroup.
- **Authentication is not correctness.** A valid tag proves a member's radio sent the packet, not
  that its GPS is sane. Receivers keep the last fix per slot and mark a rider "position
  implausible" on an impossible speed, a jump of kilometres or a fix time in the future, rather
  than trusting it or silently dropping it.
- **The 16-bit fix time unwraps unambiguously only within half its range,** 9 h 6 min
  (RFC 1982, §3.2).

## Sizing the group at creation

The organiser states **how many riders** the group will have and **how fresh** positions must be.
Everything else follows, and goes into the group QR:

- the frame is (riders + spare slots) × slot;
- the mode is the longest-range one whose frame still gives every rider a position that often,
  without any rider passing the 10 % duty cycle;
- spare slots, about one per ten riders, are for late joiners. More riders than slots means a new
  QR, or dynamic slots in a later version.

`tools/airtime.py` (`plan()`) [calc]:

| Positions every | 5 riders | 10 riders | 15 riders | 20 riders |
|---|---|---|---|---|
| ≤ 2 s | SF10/250, 150.5 dB, 1.9 s | SF9/250, 148 dB, 1.2 s | SF9/250, 148 dB, 1.9 s | SF8/250, 145.5 dB, 1.5 s |
| ≤ 5 s | SF10/125, 153.5 dB, 3.7 s | SF10/125, 153.5 dB, 4.2 s | SF9/125, 151 dB, 3.7 s | SF9/125, 151 dB, 4.7 s |
| ≤ 10 s | SF10/125, 153.5 dB, 3.7 s | SF10/125, 153.5 dB, 4.2 s | SF10/125, 153.5 dB, 6.5 s | SF10/125, 153.5 dB, 8.4 s |

The trade-off this exposes:

- **A small group buys range.** Five to ten riders at about 4 s get SF10/125, whose link budget
  (153.5 dB) matches Meshtastic's LongFast. On LongFast itself, ten riders would get a position each
  every ~55 s with relaying, or every ~27 s without [calc].
- **A large group pays for cadence with range.** Twenty riders every 1.5 s need SF8/250, about
  8 dB less budget.
- **For very small groups the duty cycle, not the frame, sets the pace.** Five riders at SF10/125
  fit a 2.3 s frame, but each rider may only send every 3.7 s.

What `plan()` leaves out:

- **It assumes exactly 24 bytes.** Since airtime grows in steps (packet sketch above), the final
  length can move a cell to the next mode.
- **It budgets each rider's own positions only.** A rider who also relays or sends a route spends
  the same duty allowance.
- **Unlike the capacity table, it reserves no further 20 % of the frame.**
- **Its duty-cycle stretch for small groups is idle time the real frame must include.**
- **Two 125 kHz channels exactly fill the 250 kHz band** and leave no edge margin. Occupied
  bandwidth and oscillator error need checking (ETSI EN 300 220-2, §§4.4.4–5, 4.4.8) before treating
  them as two independent channels.

## Sharing a route

**DMD:** the LoRa format we recovered has only two packet types, positions and chat
([04](04-dmd-protocol.md)). DMD2's route sharing therefore goes through its server, not the radio
(inferred from the packet types, not checked against DMD2's code).

**Over LoRa it is feasible.** Measured on two real routes with `tools/route_size.py` [calc]:

- the owner's multi-day route: 4.0 MB of GPX, 0.84 MB gzipped, 48 967 points over 4 926 km
  (input SHA-256 `24d1aa6b681502b5…`);
- the public Trans-Morocco Trail map v3.9, mostly off-road: 6.9 MB, 1.15 MB gzipped, 99 503 points
  over 3 160 km (`aac93acfc73bd47c…`).

GPX's megabytes are XML text, timestamps and dense points, not information. The tool simplifies
each line, keeps its order of travel, encodes it as binary deltas on a 1e-5 degree grid, and
measures the decoded line's actual worst distance from the original. Sizes include a 24-byte route
manifest; a packet carries 230 bytes of route. Per 200 km, with the tool's default tolerances (how
to reproduce: [07](07-experiments.md)):

| Worst distance allowed | Multi-day route | Trans-Morocco | Packets |
|---|---|---|---|
| every point | 5.3 kB | 13.3 kB | 23–58 |
| 2 m (achieved 1.7–1.8 m) | 3.5 kB | 7.1 kB | 16–31 |
| 5 m | 2.7 kB | 4.4 kB | 12–20 |
| 10 m | 2.3 kB | 3.2 kB | 10–15 |
| 20 m | 1.7 kB | 2.3 kB | 8–11 |
| 50 m (mapped roads only) | 1.2 kB | 1.4 kB | 6 |
| 100 m (mapped roads only) | 0.81 kB | 0.85 kB | 4 |
| 200 m (mapped roads only) | 0.53 kB | 0.50 kB | 3 |
| 500 m (mapped roads only) | 0.28 kB | 0.23 kB | 2 |

Even every point, in binary on the grid, is about 30 times smaller than the GPX; gzip manages 5–6.
Their 135 and 174 waypoints (fuel, camps) fit in 4–5 packets at 6 bytes each. A route packet, with
16 bytes of header and tag on top, takes 192 ms on air at SF7/250, 0.60 s at SF9/250 and 2.21 s at
SF10/125.

**Airtime is not the whole cost: the duty budget is** [calc]. Take a 200 km mountain route at about
10 points per km: 5 kB, 22 packets, 48.7 s on air at SF10/125 (`tools/airtime.py`).

- On its own, at a sustained 10 %, it takes 487 s, about 8 minutes.
- A group already sending positions every 4 s at SF10/125 spends 9.27 % of each rider's hour on
  them, which leaves 26.4 s an hour for anything else. At that rate the route takes about
  111 minutes to fit.

Unused budget from the past hour may allow an earlier burst: these are average rates, not
mandatory delays. Retries count too. Big transfers belong before the ride, or in a fast mode.

### Coarse points, road snapping, and their limits

**Coarse points work on roads because OsmAnd re-snaps them.** Its AIDL V2 `navigateGpx` takes
`snapToRoad`, `snapToRoadMode` and `snapToRoadThreshold` [code], so a track with a point every
1–4 km can be turned back into a road-following route on each phone: a day's route in 2–4 packets.

**Off-road, coarse points and snapping are dangerous.** A simplified line can cut a switchback,
and snapping can move the route onto a neighbouring trail, onto tarmac, or towards a drop. Rules:

1. **Off-road, and anywhere not on a mapped road, the track goes out exact or within 2 m, and is
   never snapped.**
   - Within 2 m (achieved 1.81 m on the Trans-Morocco Trail) means the decoded line, after
     rounding, never strays further than that from the original, and doubling back is kept in
     order.
   - "Exact" refers to the decoded geometry. It says nothing about whether the trail itself is
     safe to ride.
   - Trans-Morocco, per 200 km: every point is 13.3 kB, 58 packets (11 s on air at SF7/250, 35 s at
     SF9/250, 128 s at SF10/125). Within 2 m it is 7.1 kB, 31 packets (6 s, 19 s, 69 s).
2. **Coarse points with snapping are an optimisation, never the default,** and only for road
   segments that pass a check on the receiving side.
   - A sender-only check cannot vouch for receivers whose maps or routing profiles differ.
   - Each receiver snaps the segment and compares the result with the approved geometry. If it
     strays by more than about 10 m, it asks for the dense geometry instead.
3. **An authenticated manifest:** route ID, segments and their count, the geometry's hash,
   revision, and where snapping is allowed. A hash proves the bytes match; it does not prove a road
   choice is safe.
4. **The rider accepts a route before navigation changes.** Each app says "exact route received" or
   "road-snapped approximation", and never shows a partial route as complete.
5. **Before the ride, hand the full GPX over phone to phone.** LoRa is for changes on the way.

### Geometry and intent

**A route is geometry plus intent, sent separately.** The same GPX can be ridden in either
direction, joined part-way, or used one segment (one day) at a time, and the plan changes on the
road. So:

- **Geometry** is the big part: segments, each marked exact or road-snapped. It is sent once and
  cached by its hash. A rider who missed it asks for it again by that hash.
- **Intent** is one small message, about 10 bytes:

  | Field | Contents |
  |---|---|
  | route | the geometry's hash, ~6 bytes |
  | segment or range | which part of the route, typically one day |
  | direction | forward or reverse |
  | snapping | allowed or not; only ever on verified road segments |
  | version | which revision of the plan this is |
  | from the start | optional flag; by default nobody needs to start at the beginning (below) |

  "We turn back" or "tomorrow is segment 3" is then a few bytes, not a new route.

**Joining part-way.** When a rider is off the track, OsmAnd leads them back onto it, so a group can
start anywhere along a route [owner]. The join is not simply the nearest point:
`TrackStartPointFinder` also weighs the rider's bearing and previous progress, and
`passWholeRoute = true` starts from the beginning [code]. Two consequences:

- **the points must be in the order of travel,** which is why direction is part of the intent;
- **loops and out-and-back tracks need a preview** of where navigation will start.

`navigateGpx` offers no reverse or segment choice, so our app builds the GPX it hands over: reversed
and cut to the segment.

### Delivering it to everyone

**The hard part is delivery**, not airtime: in a broadcast, each rider misses different packets.

- **Route packets need slots of their own.** A 246-byte route packet takes 192 ms at SF7/250 and
  cannot fit a 41 ms position slot. Neither can a 70-byte relay packet (64 ms). The frame needs
  typed, larger slots, or smaller fragments, and cadence, per-rider duty and feedback overhead then
  have to be recomputed. The sizes [calc]:
  - at SF7/250 a route packet is 4.7 position slots, 18.8 % of a 20-rider frame, so the frame's
    free 20 % holds one;
  - at SF10/125 one route packet is half of a 10-rider frame (2.21 s of 4.19 s), though only
    0.06 % of the sender's hour.

  In the long-range modes a route therefore goes before the ride, or in a burst in a faster mode.
  A burst needs every receiver to switch at the same moment, and the far riders a slow mode was
  chosen for are the ones that miss it.
- **Feedback rides in the position packet:** the route version and a "have it all" bit (in the
  sketch above), or a small bitmap of missing parts. "No feedback heard" is tracked separately from
  "complete".
- **The sender repeats only what someone still lacks,** with bounded retries and an expiry. A help
  message always goes before a route transfer.
- **Forward error correction is the alternative**: a fountain code sends a few extra packets. It
  is not an unconditional "any N of N+k" guarantee; it needs a chosen code and failure target
  (RFC 6330, §5.4.2).

**In OsmAnd** [code]:

- a received route is imported and shown with `importGpx` and `showGpx` (the `TRACKS_EDIT` group),
  and navigated with `navigateGpx` (the `NAVIGATION` group). Neither group is granted by default,
  so the rider allows them once;
- via points can go on our own layer instead (the `MAP` group) with a "navigate" button.

**Meshtastic** has waypoints (single points with an expiry) but no route type. A route there would
be a series of packets on a private port, without reliable delivery to the whole group: possible,
but clumsy.

## v2: relaying

Keep one or two **relay slots** per frame from the start, so relaying can be added without changing
the frame:

- **Who relays:** a rider in the middle of the column, chosen from what each rider hears.
- **What a relay packet carries:** riders it heard recently that others may not have, each as an
  offset from the relay's own position plus the original fix time. About 6 bytes per rider, so ten
  riders fit in about 70 bytes: 64 ms at SF7/250, so a relay slot is a larger slot.
- **Freshness:** a relayed position keeps its original fix time, so it never looks younger than it
  is. That is the "retelling" DMD needed, done with bounded airtime and honest ages.
- **Budget:** the relaying rider's own duty cycle carries the relay slots too.

## Frequency and PHY

| Option | Conditions [docs] | Notes |
|---|---|---|
| 869.40–869.65 MHz | 10 % duty, 500 mW ERP (or a qualifying access technique instead of duty cycle) | Meshtastic sits at 869.525 MHz (250 kHz) and MeshCore at 869.618 MHz (62.5 kHz) in the same band. Pick SF and bandwidth to suit, and measure interference with the RTL-SDR |
| 868.0–868.6 MHz | 1 % duty, 25 mW ERP | one rider every ~3 s at SF7 at best |
| 169.400–169.475 MHz | 1 % duty, 500 mW ERP, only 75 kHz wide (general SRD, Decision 2025/105 row 37c) | our 125/250 kHz modes do not fit; Garmin's approval says nothing about our waveform |

The actual country, application category, antenna ERP and occupied bandwidth come before any of
these. Moving between frequencies in one band does not start a fresh duty allowance
(ETSI EN 300 220-1, §5.4).

A shorter preamble saves airtime: 8 symbols down to 5 is 5 % at SF7/250 and 6.6 % at SF10/125
[calc], at some cost in detection that E2 and E6 measure. Bike-powered nodes can listen all the
time, so no receive duty cycling is needed in v1.

## Hardware, firmware, app

- **Radio:** an SX1262 driven directly: Heltec V3/V4, LILYGO T-Deck, Seeed Wio Tracker L1, RAK4631.
  EBYTE modules are out, since their UART gives no control over the exact moment of transmission.
- **Firmware:** start from
  [dmd2-lora-bridge](https://github.com/MiklerGM/dmd2-lora-bridge) (Apache-2.0). Reusable as is:
  - the platform layer for ESP-IDF and Zephyr (nRF52840);
  - BLE to the phone;
  - the bench tools: `bench/interleave.py` and the SDR scripts.

  Its radio backend is **not** a TDMA modem yet: it fixes SF11/500, forced LDRO and EBYTE's sync
  word and framing [code, `firmware/src/sx1262.c`]. Generic PHY timing and power have to be measured
  anew.
- **GPS:** on the node, with its PPS line where the board has one (Timing above); the phone's
  position over BLE as a fallback.
- **App:** the OsmAnd side as in MeshAnd ([05](05-osmand-integration.md)); reusing its code makes
  ours GPL-3.0.
- **A control experiment:** the SX1262 also does GFSK up to 300 kbit/s [docs]. If a shorter-range
  mode meets the measured riding requirement, a direct-range GFSK comparison shows how much of any
  gain is the new protocol and how much is just a faster PHY.

## Costs and risks

- firmware on every board we support, and its maintenance;
- field testing with several riders before it can be trusted;
- no interoperability with riders who use plain Meshtastic;
- a band shared with other networks;
- doing the regulatory homework ourselves: the band limits of [02](02-radio-basics.md), and the
  Radio Equipment Directive (2014/53/EU) for a radio running our firmware, a smaller job on a
  certified off-the-shelf board than on one we build;
- riders' tracks are personal data: what a companion logs, for how long and who sees it are design
  choices, though a private group ride likely falls under GDPR's household exemption
  (Art. 2(2)(c));
- the security design above, done properly.

## Open design questions

- Riders who join late, or a roster that changes mid-ride: the activation epoch and the spare-slot
  allocator in practice, and who hands out slots when the organiser's radio is gone.
- Keeping time when nobody has a GPS fix (tunnels, dense forest) and on boards without PPS, and how
  long a radio may stay silent before it must resynchronise.
- The nonce: a random or persisted session, or the frame number.
- The fix time: whole seconds, or the fix's age at the slot in fractions of a second.
- Key changes mid-ride: per-device keys would allow a rekey that leaves a lost radio out.
- Two groups using the same schedule in the same place: offset the frame or the frequency by the
  group key?
- A fixed frame versus adaptive cadence: a variable frame, or skipping slots when stopped?
- Text and "need help" in the schedule: a spare slot, or a flag plus a follow-up, and how help is
  acknowledged.
- The tag length, and the key lifetime it implies.
- Typed slots for routes and relays: sizes, and how often.
- Receiving all the time on bike power versus duty-cycled reception on battery.
