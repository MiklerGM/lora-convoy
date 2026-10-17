# 06. A protocol of our own: design notes

**Status:** notes, not a specification. Nothing here is implemented or measured; every capacity
figure is [calc].

## When it is worth building

Tuned Meshtastic with an OsmAnd companion ([03](03-alternatives.md), [05](05-osmand-integration.md))
is the baseline. A protocol of our own pays off only if the requirements in [01](01-goals.md) go
beyond what that baseline can do:

- more than about 10 riders who each need a position every 5 s or more often;
- positions more often than every 5 s for anyone;
- long, stretched columns where flooding does not hold the group together.

If none of these applies, building our own means paying for firmware on every board, field
testing, and losing interoperability with plain Meshtastic users, for no gain.

> **Review (Codex, 2026-10-17):** Inference: the third trigger is not solved by the proposed v1:
> it has no relay at all. Compare a stretched column against a measured Meshtastic baseline
> **after** v2 forwarding exists; faster direct broadcasts do not cross an obstructing ridge.

## What to avoid: "DMD, but better"

DMD's core is random access plus every device retelling every rider, at a fixed fast pace
([04](04-dmd-protocol.md)). Load then grows with the square of the group. Smaller packets or real
encryption on top of that design would not change this, so a better DMD is the wrong target.

## Principles

1. **Scheduled access.** Each rider owns a time slot in a repeating frame (TDMA), timed by GPS. No
   collisions inside the group, including between riders who cannot hear each other, and the
   channel can be used to 70–80 % instead of the 25 % random access tolerates.
2. **Each rider sends only their own position**, in a small binary packet.
3. **Relaying is separate and selective**: dedicated slots, only where needed (v2).
4. **One clock: GPS time.** A position carries its fix time; age is now minus that. A relayed
   position never gets younger.
5. **Cadence adapts**: faster when moving and close together, slower when stopped.
6. **Authenticated encryption** with a group key.
7. **Inside the sub-band's limits** for duty cycle and power.

## Capacity [calc]

Group capacity in positions per second; 24-byte packet, 8-symbol preamble, 10 ms guard, 80 % of
the frame used for slots:

| Approach | Positions/s for the group | 10 riders | 20 riders |
|---|---|---|---|
| DMD, stock | channel full at 4–5 riders | — | — |
| Meshtastic ShortFast, one relay, 25 % load | 2.2 | every ~5 s | every ~9 s |
| Meshtastic MediumFast, one relay | 0.7 | every ~15 s | every ~31 s |
| Slots by GPS time, SF7/250 | ~20 | every 1 s | every 1 s |
| Slots by GPS time, SF8/250 | ~12 | every 1 s | every ~1.7 s |
| Slots by GPS time, SF9/250 (range like MediumFast) | ~7 | every ~1.5 s | every ~2.8 s |

One rider sending every second is on air 3.1 % of the time at SF7, 5.7 % at SF8 and 10.3 % at
SF9, which is the limit of the 869.4–869.65 MHz band.

> **Review (Codex, 2026-10-17):** [calc] Independently derived explicit-header airtimes are
> 30.848 / 56.576 / 102.912 ms for SF7/8/9. With 10 ms guard and the stated 80% slot budget,
> 20 riders require **1.021 / 1.664 / 2.823 s** respectively, before additional traffic;
> “every 1 s” is rounding beyond this model at SF7. SF9 every second is **over**, not within,
> the 10% duty-cycle option (minimum 1.02912 s with no other TX). Derivation and assertions:
> [`tools/review_checks.py`](../tools/review_checks.py), independently checked against
> [Semtech `src/sx126x.c`](https://github.com/Lora-net/sx126x_driver/blob/a10c5dfdf89788c6ac805e9fe98889de44175aa2/src/sx126x.c),
> commit `a10c5dfdf89788c6ac805e9fe98889de44175aa2`.

## v1: up to 20 riders, no relaying

The simplest version that could work.

- **The roster comes from a group QR code**, created once by the organiser. It holds:
  - the group key;
  - radio parameters (frequency, SF, bandwidth);
  - the frame length;
  - each rider's slot number.
- **The frame** is N slots, with N the roster size plus one or two spare slots for late joiners.
  A slot is the packet's time on air plus a guard. Frames start on GPS-second boundaries.
- **Synchronisation:**
  - normally from the node's own GPS;
  - without a fix, from any packet heard, since its slot number fixes where the frame started;
  - the phone's clock over BLE as a last resort.
- **Out of range means stale, not lost:** a rider beyond direct range keeps their last point on
  everyone's map, with a growing age, until they come back. Range is direct radio range: around
  1 km in forest, 2–4.5 km on an open road at SF7, up to about 6 km at SF9
  ([02](02-radio-basics.md)).

> **Review (Codex, 2026-10-17):** Inference: define how each physical radio selects its unique
> rider/slot from the common QR roster, and authenticate a roster version plus its
> activation epoch; a restarted or late node must listen before transmitting. Reserve one
> allocator for spare slots, otherwise two isolated joiners can select the same “free” slot.
> A changed frame must activate at an agreed future epoch, or old/new schedules overlap.

> **Review (Codex, 2026-10-17):** [docs] GNSS serial messages arrive after fix computation;
> the time-pulse pin is the precision timing reference ([u-blox receiver description, §12](https://content.u-blox.com/sites/default/files/products/documents/u-blox6-GPS-GLONASS-QZSS-V14_ReceiverDescrProtSpec_%28GPS.G6-SW-12013%29_Public.pdf)).
> Inference: verify PPS wiring on each board, timestamp a defined RF event, and account for
> packet duration when recovering frame phase from authenticated, replay-checked reception.
> Define GPS versus UTC, an epoch
> and `epoch + k × frame_length`: a 1.2 s frame cannot restart on every integer second.
> [calc] Two clocks each at ±20 ppm can consume a 10 ms relative guard in 250 s without resync.
> Inference: transmit only while timing uncertainty fits the guard; a phone clock over BLE
> needs an error bound before it can safely recover the schedule. External groups can still collide.

Packet sketch, about 18 bytes; with a fixed length the LoRa header can be implicit, saving airtime:

| Bytes | Field |
|---|---|
| 1 | version (2 bits), type (2 bits), flags (4 bits) |
| 1 | slot / rider index |
| 2 | GPS time of the fix, seconds modulo 65 536 (wraps every 18 h) |
| 3 + 3 | latitude, longitude, 24 bits each (~2.4 m, as DMD) |
| 1 | speed, km/h |
| 1 | course, 2° steps |
| 1 | status: help, stopped, GPS ok, battery low |
| 4 | message authentication code (AES-CCM), nonce derived from group, slot and time, so no nonce on air |

> **Review (Codex, 2026-10-17):** [docs][calc] **Do not derive a CCM nonce from fix time.** It
> repeats when the fix stalls, and the transmitted time wraps after 65 536 s (18 h 12 min 16 s).
> [NIST SP 800-38C, §5.3](https://nvlpubs.nist.gov/nistpubs/legacy/sp/nistspecialpublication800-38c.pdf)
> requires uniqueness per key. Inference: use a distinct transmission counter with a
> reboot-safe session/epoch and sender identity, authenticate the header, and keep replay state;
> carry fix time separately. Define how receivers recover the epoch after long outages.
> [docs] A 32-bit tag needs an explicit failed-verification/key-lifetime budget; NIST Appendix B
> requires risk analysis below 64 bits. Budget those fields before fixing packet length.

> **Review (Codex, 2026-10-17):** [docs] A shared key authenticates membership, not an
> individual rider: every member can construct another slot's packet ([RFC 3610, §2.1](https://www.rfc-editor.org/rfc/rfc3610#section-2.1)).
> Inference: state that trust boundary, rotate keys when a device is lost or a rider leaves,
> and authenticate route/control messages too. A modulo fix time also needs bounded unwrapping;
> half the 16-bit range is only 9 h 6 min 8 s ([RFC 1982, §3.2](https://www.rfc-editor.org/rfc/rfc1982#section-3.2)).

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
- **For very small groups the duty cycle, not the frame, sets the pace.** Five riders at
  SF10/125 fit a 2.3 s frame, but each rider may only send every 3.7 s.

The 869.40–869.65 MHz band holds two 125 kHz channels, so the group key could also pick one of them.

> **Review (Codex, 2026-10-17):** [calc] `plan()` budgets each rider's **own positions only**;
> a relay or route sender must add all its traffic to the same duty ledger. Unlike the capacity
> table, `plan()` does not reserve a further 20% of the frame beyond its spare slots. Its small-group
> duty stretch also creates idle time outside `(riders + spare) × slot`, which the actual frame
> definition must include. [docs] Two nominal 125 kHz channels exactly fill 250 kHz and leave
> no nominal edge margin; compliant occupied bandwidth and oscillator error need checking
> ([ETSI EN 300 220-2 V3.3.1, §§4.4.4–5, 4.4.8](https://www.etsi.org/deliver/etsi_en/300200_300299/30022002/03.03.01_60/en_30022002v030301p.pdf)).
> Inference: they are not two proven independent channels merely because the arithmetic fits.

## Sharing a route

**DMD:** the LoRa format we recovered has only two packet types, positions and chat
([04](04-dmd-protocol.md)). DMD2's route sharing therefore goes through its server, not the radio
(inferred from the packet types, not checked against DMD2's code).

**Over LoRa it is feasible** [calc]; 230 bytes of route per packet, plus 16 for header and MIC:

| What is sent | Size | On air once: SF7/250 · SF9/250 · SF10/125 |
|---|---|---|
| **Via points only** (20); each phone's OsmAnd computes the route on its own offline map | 130 B, 1 packet | 0.1 · 0.4 · 1.4 s |
| Simplified track, highway, 200 km (~2 points/km, 2.5 B/point after delta coding) | ~1 kB, 5 packets | 1.0 · 3.0 · 11.1 s |
| Simplified track, mountain roads, 200 km (~10 points/km) | ~5 kB, 22 packets | 4.2 · 13.3 · 48.7 s |

Even the worst case fits the 10 % duty cycle comfortably. Via points are by far the cheapest; the
risk is that riders whose maps or routing profiles differ get slightly different routes, which is
acceptable for "ride via these points".

> **Review (Codex, 2026-10-17):** [calc] Airtime alone does not determine legal completion
> time: 48.7 s of TX consumes **487 s of replenishment** at a sustained 10% budget with no
> other traffic. At SF10/125 a position every 4 s already uses 9.2672%; only 26.38 s/hour
> remains for route traffic, so 48.71 s takes about **111 min to replenish** at that rate.
> An unused hourly budget may allow an earlier burst; these are amortized rates, not mandatory
> burst delays. Include retries and the actual observation-window ledger. Independent values are in
> [`review_checks.py`](../tools/review_checks.py); regulatory accounting is reviewed in [02](02-radio-basics.md).

**Measured on two real routes** (2026-10-17, `tools/route_size.py`) [calc]:

- the owner's multi-day route: 4.0 MB of GPX, 0.84 MB gzipped, 48 967 points over 4 920 km;
- the public Trans-Morocco Trail map v3.9, mostly off-road: 6.9 MB, 1.15 MB gzipped, 99 503 points
  over 3 157 km.

GPX's megabytes are XML text, timestamps and dense points, not information. Simplified, then
written as binary deltas. The coarse tolerances (50–500 m) are **for mapped roads only**; see the
off-road rules below.

| Douglas-Peucker tolerance | Multi-day route (4 920 km) | Trans-Morocco (3 157 km) | Per 200 km |
|---|---|---|---|
| 10 m | 54 kB, 236 packets | 49 kB, 214 packets | ~2.2–3.1 kB, 10–14 packets |
| 50 m | 28 kB, 124 packets | 21 kB, 93 packets | ~1.2–1.4 kB, 6 packets |
| 100 m | 20 kB | 13 kB | ~0.8 kB, 4 packets |
| 200 m | 13 kB | 7.8 kB | ~0.5 kB, 3 packets |
| 500 m | 6.7 kB | 3.6 kB | ~0.25 kB, 2 packets |

That is 100–300 times smaller than the GPX, where gzip manages about 5. Their 135 and 174 waypoints
(fuel, camps) fit in 4–5 packets at 6 bytes each.

**Coarse points work because OsmAnd re-snaps them to roads.** Its AIDL `navigateGpx` takes
`snapToRoad`, `snapToRoadMode` and `snapToRoadThreshold` (`NavigateGpxParams`) [code], so a
received track with a point every 1–4 km can be turned back into a road-following route on each
phone. A day's route is then 2–4 packets, under a second on air at SF7/250.

**Off-road, coarse points and snapping are dangerous.** A simplified line can cut a switchback,
and snapping can move the route onto a neighbouring trail, onto tarmac, or towards a drop. The
coarse rows above are for mapped roads only. Rules:

1. **Off-road, and anywhere not on a mapped road, the track goes out exact or near-exact, and is
   never snapped.** Douglas-Peucker guarantees the simplified line never strays from the original
   by more than its tolerance. At 2 m, which is less than a GPS's own error, it only drops redundant
   points along straight runs. The receiving app draws exactly what was sent. Measured on the
   Trans-Morocco Trail, per 200 km:

   | Off-road track | Size | On air once: SF7/250 · SF9/250 · SF10/125 |
   |---|---|---|
   | every point, on a 1.1 m grid | 13.3 kB, 58 packets | 11 · 35 · 128 s |
   | at most 2 m from the original | 5.9 kB, 26 packets | 5 · 16 · 58 s |
   | at most 5 m from the original | 4.1 kB, 18 packets | 3.5 · 11 · 40 s |

   All of these fit the 10 % duty cycle, so there is no reason to compress further.

   > **Review (Codex, 2026-10-17):** [code][calc] The current tool cannot guarantee the stated
   > end-to-end 2 m bound: a three-point example simplified at 2 m becomes **2.449 m** away
   > after its 1e-5° rounding. A collinear out-and-back is also reduced to its endpoints,
   > losing traversal order despite zero geometric deviation. Reproductions:
   > [`review_checks.py`](../tools/review_checks.py); baseline `tools/route_size.py`,
   > `simplify()` / `encoded_size()`, commit `fe4c836018fe91683540dc30c5e24aca1e9ce1cb`.
   > Inference: budget quantization/projection error, preserve turns and traversal order,
   > and label “exact” relative to the decoded geometry, not physical trail safety.
2. **Coarse points with road snapping are an optimisation, never the default.** They are used only
   for segments the sender has verified:
   1. it snaps the coarse points with its own OsmAnd, exactly as the receivers will;
   2. the result stays within about 10 m of the original over the whole segment.

   Anything else goes out dense.

   > **Review (Codex, 2026-10-17):** Inference: a sender-only snap check cannot validate
> receivers with different maps or profiles. Verify each receiver's reconstruction against
> the approved geometry; otherwise request dense geometry. A hash proves matching bytes, not a safe
   > road choice; authenticate the manifest (route ID, segments, count, hash, revision and
   > allowed snapping) and require rider acceptance before navigation changes.
3. **Integrity.** A route carries a hash of its decoded geometry. Each rider's app says "exact
   route received" or "road-snapped approximation", and never shows a partial route as complete.
4. **Before the ride, hand the full GPX over phone to phone.** LoRa is for changes on the way.

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
- **Joining part-way is OsmAnd's job.** When a rider is off the track, OsmAnd leads them to the
  nearest point of it, so a group can start anywhere along a route [owner]. What it needs is the
  points in the order of travel: joined in the middle of a track that runs the other way, it would
  lead towards the track's start. Hence direction is part of the intent.
- **In OsmAnd** [code], `navigateGpx` offers no reverse or segment choice (`NavigateGpxParams`:
  `passWholeRoute`, snapping, `force`). Our app builds the GPX it hands over (reversed and cut to
  the segment), so that does not matter.

> **Review (Codex, 2026-10-17):** [code] OsmAnd's join is not a universal nearest-point
> guarantee: `TrackStartPointFinder` also uses bearing/prior progress, and `passWholeRoute=true`
> selects the beginning. Loops and out-and-back tracks therefore need a departure preview.
> The snapping fields belong to **AIDL V2**, not legacy
> `net.osmand.aidl.navigation.NavigateGpxParams` (both parameter classes are pinned in [05](05-osmand-integration.md)).
> Sources at commit `a68b83f322aa1bf76c8a98a2d76da261d59b15f7`:
> [`TrackStartPointFinder.java`](https://github.com/osmandapp/OsmAnd/blob/a68b83f322aa1bf76c8a98a2d76da261d59b15f7/OsmAnd-java/src/main/java/net/osmand/router/TrackStartPointFinder.java),
> [`GpxRouteHelper.java`](https://github.com/osmandapp/OsmAnd/blob/a68b83f322aa1bf76c8a98a2d76da261d59b15f7/OsmAnd/src/net/osmand/plus/routing/GpxRouteHelper.java).

**The hard part is delivering it to everyone**, not airtime: in a broadcast, each rider misses
different packets. Slots make this natural:

- one route packet per frame, in a spare slot, so positions keep flowing;
- every position packet carries the route's version and a "have it all" bit, or a small bitmap of
  missing parts;
- the sender repeats only what someone still lacks. Alternatively, forward error correction (a
  fountain code): send a few extra packets, so that any N of N+k rebuild the route.

> **Review (Codex, 2026-10-17):** [calc] A 246-byte route packet at SF7/250 takes **192.128 ms**;
> it cannot occupy a spare 40.848 ms position slot. Likewise, a 70-byte relay needs 64.128 ms.
> Inference: define larger typed slots or smaller fragments, then recompute cadence, individual
> duty cycle and feedback overhead. The current position sketch contains no route version or
> missing-parts bitmap. Track “no feedback” separately from “complete”, bound retries/expiry,
> and prioritize help over a route transfer. [docs] Fountain decoding is not an unconditional
> “any N” guarantee; choose a code and failure target ([RFC 6330, §5.4.2](https://www.rfc-editor.org/rfc/rfc6330#section-5.4.2)).

**Before the ride**, a route is easier to hand over phone to phone (Wi-Fi, Bluetooth, file sharing);
LoRa is for changes on the way ("the pass is closed, new route").

**In OsmAnd** [code]:

- a received route can be imported and shown (`importGpx`, `showGpx`: the `TRACKS_EDIT` group) and
  navigated (`navigateGpx`: the `NAVIGATION` group). Neither group is granted by default, so the
  rider allows them once;
- via points can also go on our own layer (the `MAP` group, granted by default) with a "navigate"
  button.

**Meshtastic** has waypoints (single points with an expiry) but no route type. A route there would
be a series of packets on a private port, without reliable delivery to the whole group: possible,
but clumsy.

## v2: relaying

Keep one or two **relay slots** per frame from the start, so relaying can be added without changing
the frame:

- **Who relays:** a rider in the middle of the column, chosen from what each rider hears.
- **What a relay packet carries:** riders it heard recently that others may not have, each as an
  offset from the relay's own position plus the original fix time. About 6 bytes per rider, so ten
  riders fit in about 70 bytes.
- **Freshness:** a relayed position keeps its original fix time, so it never looks younger than it
  is. That is the "retelling" DMD needed, done with bounded airtime and honest ages.

## Frequency and PHY

| Option | Duty cycle and power | Notes |
|---|---|---|
| 869.4–869.65 MHz | 10 %, 500 mW | Meshtastic sits at 869.525 MHz (250 kHz) and MeshCore at 869.618 MHz (62.5 kHz) in the same band. Pick SF and bandwidth to suit, and measure interference with the RTL-SDR |
| 868.0–868.6 MHz | 1 %, 25 mW | one rider every ~3 s at SF7 at best |
| 169.4 MHz, like Garmin | 500 mW in EMEA | better range per watt; needs different radio hardware |

> **Review (Codex, 2026-10-17):** [docs] The general EU 169.4–169.475 MHz SRD entry has
> **500 mW ERP and ≤1% duty cycle**; metering's 10% entry is a different use category. Its
> 75 kHz width also excludes the proposed 125/250 kHz modes. This is not a general EMEA
> 500 mW fallback ([Decision (EU) 2025/105, Annex, row 37c](https://eur-lex.europa.eu/eli/dec_impl/2025/105/oj/eng/pdf)).
> Inference: fix the actual country, application category, antenna ERP and occupied bandwidth
> before choosing a profile. Switching frequencies within one operational band does not
> create a fresh duty allowance ([ETSI EN 300 220-1, §5.4](https://www.etsi.org/deliver/etsi_en/300200_300299/30022001/03.01.01_60/en_30022001v030101p.pdf)).

A short preamble saves airtime. Bike-powered nodes can listen all the time, so no receive duty
cycling is needed in v1.

## Hardware, firmware, app

- **Radio:** an SX1262 driven directly: Heltec V3/V4, LILYGO T-Deck, Seeed Wio Tracker L1, RAK4631.
  EBYTE modules are out, since their UART gives no control over the exact moment of transmission.
- **Firmware:** start from
  [dmd2-lora-bridge](https://github.com/MiklerGM/dmd2-lora-bridge) (Apache-2.0). It already has:
  - the SX1262 driver, on a platform layer for ESP-IDF and Zephyr (nRF52840);
  - BLE to the phone;
  - the bench tools: `bench/interleave.py` and the SDR scripts.
- **GPS:** on the node (the L1 has one); the phone's position over BLE as a fallback.
- **App:** the OsmAnd side as in MeshAnd ([05](05-osmand-integration.md)); reusing its code makes
  ours GPL-3.0.

> **Review (Codex, 2026-10-17):** [code] Reusing the bridge is feasible, but its radio backend
> currently fixes SF11/BW500, forced LDRO and the EBYTE sync/framing: it is not already a
> configurable TDMA modem ([`firmware/src/sx1262.c`](https://github.com/MiklerGM/dmd2-lora-bridge/blob/2234b5a4077479bf4145c86d76d4061af515bd6f/firmware/src/sx1262.c),
> commit `2234b5a4077479bf4145c86d76d4061af515bd6f`). Inference: retain the BLE/platform code,
> but measure generic PHY timing and power independently. [docs] The same SX1262 also supports
> GFSK up to 300 kbit/s ([Semtech](https://www.semtech.com/products/wireless-rf/lora-connect/sx1262));
> inference: a direct-range GFSK comparison is a useful control before attributing every gain
> to a new LoRa protocol, if that shorter-range mode meets the measured riding requirement.

## Costs and risks

- firmware on every board we support, and its maintenance;
- field testing with several riders before it can be trusted;
- no interoperability with riders who use plain Meshtastic;
- a band shared with other networks;
- doing the regulatory homework ourselves.

## Open design questions

- Riders who join late, or a roster that changes mid-ride.
- Keeping time when nobody has a GPS fix (tunnels, dense forest).
- Two groups using the same schedule in the same place: offset the frame or the frequency by the
  group key?
- A fixed frame versus adaptive cadence: a variable frame, or skipping slots when stopped?
- Text and "need help" in the schedule: a spare slot, or a flag plus a follow-up?
- Receiving all the time on bike power versus duty-cycled reception on battery.
