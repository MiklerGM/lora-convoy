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
