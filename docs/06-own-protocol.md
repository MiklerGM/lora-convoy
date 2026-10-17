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

- **Each physical radio takes exactly one roster entry.** The roster carries a version and an
  activation epoch, both authenticated. A changed roster or frame takes effect at an agreed future
  epoch, or old and new schedules overlap.
- **Spare slots** (about one per ten riders) have a single allocator, the organiser's radio.
  Otherwise two late joiners who cannot hear each other pick the same "free" slot.
- **A radio that restarts or joins late listens for a frame before it transmits.**

### Timing

- **Frames are numbered from an epoch:** frame k starts at `epoch + k × frame_length`, in GPS time,
  with GPS against UTC defined. A 1.2 s frame cannot restart on every whole second.
- **The time reference is the GPS's PPS pin, not its serial messages,** which arrive after the fix
  is computed [docs]. Each board's PPS wiring has to be checked.
- **Without a fix,** a radio recovers frame phase from authenticated, replay-checked packets it
  hears: their slot is known, and the packet's own duration is taken into account. The phone's
  clock over BLE is a last resort, and only with a known error bound.
- **Drift** [calc]: two clocks at ±20 ppm eat a 10 ms guard between them in 250 s without
  resynchronising. A radio transmits only while its timing uncertainty fits inside the guard, and
  goes quiet otherwise.

  Review (Codex qwen, 2026-10-17): the drift arithmetic is right (10 ms / 40 ppm = 250 s),
  and I think the 10 ms guard is far too large, because drift is not what it has to cover. Over a
  1.2 s frame, two clocks at ±20 ppm separate by 1.2 s × 40 ppm = **48 µs**, about 200 times less
  than the guard. The real budget components are elsewhere: PPS alignment error inside the GPS
  module (typically tens of ns to a few µs for a good receiver, but a phone or a cheap module
  driven by serial messages is much worse), the radio's RX-to-sync latency, and the time from
  detecting the slot boundary to starting the transmit. [calc, inference]

  The cost of guessing wrong is capacity. At SF7/250 a 24-byte packet is 30.848 ms, so a 10 ms
  guard is **32 % overhead**; a 2 ms guard makes the slot 32.848 ms and lifts the group from
  19.58 to 24.35 positions/s, which is 20 riders every 0.821 s instead of 1.021 s — about 24 %
  more cadence for free. At SF9/250 the same change is 7.09 → 7.63/s. [calc]

  So: state the guard as a sum of named components instead of a round 10 ms, and measure the two
  unknown ones on our boards before the frame is fixed. That belongs in E6 as an explicit
  measurement (PPS accuracy and RX wake latency per board), not as an assumption. [inference]
- **Other groups can still collide** with us; see the open questions.

### Range

Out of range means stale, not lost: a rider beyond direct range keeps their last point on
everyone's map, with a growing age, until they come back. Range is direct radio range: around 1 km
in forest (a guess), 2–4.5 km on an open road at SF7, up to about 6 km at SF9
([02](02-radio-basics.md)).

### Packet sketch, about 24 bytes

With a fixed length, the LoRa header can be implicit, saving airtime.

Review (Codex qwen, 2026-10-17): three things about this sketch, in order of how much they
matter.

**The fields do not add up to 24, and the missing byte is the one the security rules need.** The
rows sum to 17 bytes, plus a 4–8 byte tag, so the packet is 21–25 bytes depending on the tag.
More importantly, the security rules below say the CCM nonce is "group, slot, **session** and
counter", and the open questions ask how a receiver learns a sender's session after a reboot —
but there is no session field in the table. Either the session rides in the packet (costing 1–4
bytes, so the budget is not 24 yet) or it is inferred from a control message, which is a
reception dependency on the critical path of every packet after a reboot. Worth deciding before
the length is fixed, because it interacts with the tag length that is also still open. [inference]

**Drop `speed` and `course`.** Two bytes for a quantity the receiver can compute from two
consecutive fixes of the same rider, and at 6.6 % of a 30.848 ms slot those two bytes are not
free. They are also the least trustworthy fields in the packet: a stale GPS keeps repeating a speed
that is no longer true, which is exactly the failure [05](05-osmand-integration.md) warns about
for prediction. If the intent is to let a receiver extrapolate between fixes, [05] already says
why that should be marked as prediction rather than measured. Spend the bytes on the session
field above. [inference]

**"the LoRa header can be implicit, saving airtime" is true at 250 kHz and false at 125 kHz.**
Recomputing with the implicit-header flag: 24 bytes takes 28.288 ms instead of 30.848 ms at
SF7/250 (8.3 %), 51.456 instead of 56.576 at SF8/250 (9.0 %), 92.672 instead of 102.912 at
SF9/250 (10.0 %) — and **exactly the same 370.688 ms at SF10/125**, because the 20 bits saved do
not cross a symbol boundary there. [calc] That matters because `plan()` recommends SF10/125 for
exactly the small-group, long-range cases, so for those the implicit header buys nothing and the
costs stay: every node must know every packet length in advance, which is what makes the typed
route and relay slots below awkward.

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

**Security rules** [docs]:

- **The CCM nonce is built from group, slot, session and counter, never from the fix time.** A
  fix time repeats when the GPS stalls and wraps after 18 h 12 min. A nonce must never repeat under
  one key (NIST SP 800-38C, §5.3).
- **The header is authenticated**, and receivers keep replay state per sender. They must be able
  to recover the session after a long outage.
- **A 32-bit tag needs a forgery-risk and key-lifetime budget**; NIST (Appendix B) requires that
  analysis below 64 bits. Settle it before fixing the packet length.
- **A group key authenticates membership, not a rider**: any member can build another slot's
  packet (RFC 3610, §2.1). That trust boundary is stated, and keys are rotated when a device is lost
  or a rider leaves. Route and control messages are authenticated too.
- **The 16-bit fix time unwraps unambiguously only within half its range,** 9 h 6 min
  (RFC 1982, §3.2).

Review (Codex qwen, 2026-10-17): the security rules here are unusually careful for a notes
document, and I agree with all of them. Two additions.

**Authentication is not correctness.** A valid MAC proves the packet came from a genuine member's
radio; it says nothing about whether that member's GPS is sane. A member's own broken or
antenna-jammed fix, or a phone feeding a wrong position over BLE, passes the MAC and puts a
plausible-looking false rider on everyone's map. Receivers should therefore keep the last fix per
slot and reject or flag the obvious impossibilities — an implied speed far above the riding
envelope, a jump of kilometres, a fix time in the future — and mark the rider "position
implausible" rather than either trusting it or silently dropping it. Cheap, and it is the only
defence against the most likely source of a false position, which is not an attacker. [inference]

**Say what a replay window is.** "receivers keep replay state per sender" is right but unbounded:
a 3-byte counter at 1 Hz wraps in about 152 days, so replay state can be a small window, and the
window size is what bounds the damage if someone records and rebroadcasts a ride. One line with a
number (say, accept the next N counters) would make this testable in [07](07-experiments.md).
[inference]

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
| ≤ 5 s | SF10/125, 153.5 dB, 3.7 s | SF10/125, 153.5 dB, 4.2 s | SF9/125, 151 dB, 3.7 s | SF9/125, 151 dB, 4.8 s |
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
  have to be recomputed.

  Review (Codex qwen, 2026-10-17): the recomputation, so the size of this is visible. A
  position slot is 30.848 ms plus the 10 ms guard = 40.848 ms, so a 192.128 ms route packet is
  **4.70 position slots**, and one route packet per frame costs **23.5 % of a 20-rider frame** at
  SF7/250. [calc] The frame would go from 20 riders every 1.021 s to every 1.26 s for one route
  packet per frame, and a 5 kB route is 22 packets.

  The worse case is the one `plan()` recommends for small groups. At SF10/125 a route packet is
  2213.888 ms, and the 10-rider plan there uses a 4.2 s frame — so **one route packet is 52.7 %
  of the whole frame**, and it is 0.615 % of a rider's hour in one shot. [calc] Route transfer at
  the long-range modes is not a side feature that rides on spare capacity; it dominates the
  channel. That strengthens rule 5 below (hand the GPX over phone to phone before the ride) from
  advice to the only workable answer, and suggests route packets should get a temporary mode
  switch — a faster SF for a burst — rather than typed slots in the long-range frame. [inference]
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

A short preamble saves airtime. Bike-powered nodes can listen all the time, so no receive duty
cycling is needed in v1.

Review (Codex qwen, 2026-10-17): worth a number, since it is free. Each preamble symbol is
one symbol, so cutting from 8 to 5 symbols saves 3 symbols: 1.536 ms of 30.848 ms at SF7/250
(5.0 %) and 24.576 ms of 370.688 ms at SF10/125 (6.6 %). [calc] The limit is that a shorter
preamble costs detection reliability, especially with a rider who just came around a corner, so
this is an E2/E6 measurement and not a free win. Note also that the LoRa symbol count cannot go
below the receiver's own sync requirement, and DMD's own trouble with a non-standard sync pair
([04](04-dmd-protocol.md), item 7) is the cautionary example here. [inference]

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
- **GPS:** on the node (the L1 has one), with its PPS line; the phone's position over BLE as a
  fallback.
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
- doing the regulatory homework ourselves;
- the security design above, done properly.

Review (Codex qwen, 2026-10-17): "doing the regulatory homework ourselves" is doing a lot
of work in six words, and the two parts with real cost are not named anywhere in the repository:

- **RED conformity, not just band limits.** [02](02-radio-basics.md) covers the spectrum
  conditions (power, duty cycle, ERP, occupied bandwidth). Those are the *usage* rules. Placing a
  self-built radio on the market — and arguably putting a non-marketed device into service under
  one's own name — engages Directive 2014/53/EU: essential requirements for health and safety,
  EMC, and effective use of the spectrum, with a conformity assessment and a declaration. A
  modified commercial radio with a CE mark already in place is a materially smaller job than a
  board we populate ourselves, which is a real argument for the "off-the-shelf boards" requirement
  in [01](01-goals.md) beyond convenience. I am not a lawyer and this is not legal advice; the
  point is only that the plan currently budgets for the easy half. [docs, inference]
- **Personal location data.** Every rider's track is personal data, and a companion that logs
  trails (MeshAnd already keeps 30 min to 6 h of them) plus a route manifest that identifies a
  group is a processing activity with a controller — usually the organiser. Retention, who can see
  a logged track afterwards, and what happens to a rider's history when they leave the group are
  design choices, and cheap to make now rather than after a ride's tracks are public. [inference]

Neither changes the protocol, but both belong in the costs list so the decision to build is made
with them visible. [inference]

## Open design questions

- Riders who join late, or a roster that changes mid-ride: the activation epoch and the spare-slot
  allocator in practice.
- Keeping time when nobody has a GPS fix (tunnels, dense forest), and how long a radio may stay
  silent before it must resynchronise.
- Two groups using the same schedule in the same place: offset the frame or the frequency by the
  group key?
- A fixed frame versus adaptive cadence: a variable frame, or skipping slots when stopped?
- Text and "need help" in the schedule: a spare slot, or a flag plus a follow-up, and how help is
  acknowledged.
- How a receiver learns a sender's session after the sender reboots, or after a long outage: in the
  packet, or in a periodic control message.
- The tag length, and the key lifetime it implies.
- Typed slots for routes and relays: sizes, and how often.
- Receiving all the time on bike power versus duty-cycled reception on battery.
