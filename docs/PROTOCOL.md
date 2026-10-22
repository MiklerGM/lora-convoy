# Protocol

**v0, 2026-10-22: for the bench only.** Positions go in clear, so nothing is ridden on v0
([ARCHITECTURE](ARCHITECTURE.md), D11). Every part of it may change, together with the firmware
and app that use it (D3). The code is `firmware/src/phy.c`, `codec.c` and `node.c`.

## Radio mode

| | v0 |
|---|---|
| Frequency | 869.525 MHz, the middle of 869.40–869.65 MHz: 10 % duty cycle, 500 mW ERP in the EU ([08](08-regions.md)) |
| Bandwidth | 250 kHz |
| Spreading factor | 7 |
| Coding rate | 4/5 |
| Preamble | 8 symbols |
| Header and CRC | explicit header, CRC on |
| Low data rate optimisation | off; on in any mode with symbols of 16.38 ms or more |
| Sync word | SX1262 registers 0x0740 and 0x0741 = `0x64 0x74`, sync symbols 48 and 56 |
| Power | 14 dBm (25 mW) by default; 28 dBm at most on the Heltec V4.3, 22 dBm on the L1 Pro |

A 17-byte packet is on the air for 25.728 ms, by the same formula as `tools/airtime.py`.

The sync word keeps clear of its neighbours. Public LoRaWAN sends sync symbols 24 and 32, the
private default 8 and 16, Meshtastic 16 and 88, EBYTE's modules 40 and 36 (dmd2-lora-bridge,
PHY-notes). Whether other chips leave it alone is still to check on hardware (BACKLOG T-phy).

## When a board sends

- **Random access.** A board sends its position every interval, 2 s by default, drawn anew each
  time between 0.8 and 1.2 of it, so two boards that start in step drift apart. The first packet
  goes anywhere within one interval of starting.
- **Only what it knows.** Nothing before its GNSS has given a position and a time. After that, if
  the fix is lost, the last good position goes on with its own fix time, and the GPS-ok bit clear,
  so its age keeps growing on every map.
- **Within the duty cycle.** The board books each packet's time on air against the last hour, a
  minute at a time, and skips a packet that would take it past the sub-band's duty cycle (10 %).

## The position packet: 17 bytes

| Bytes | Field | Encoding |
|---|---|---|
| 0 | version, type, flags | bits 7–6 version (0), bits 5–4 type (0, position), bits 3–0 flags (0) |
| 1 | group | set by hand in v0; v1 derives a tag from the group key |
| 2 | slot | the rider, 0–31 |
| 3–5 | counter | per sender, from 0 at every start; big-endian |
| 6–7 | fix time | UTC seconds of the fix, modulo 65 536; big-endian |
| 8–10 | latitude | signed, two's complement, 2^23 = 90°: steps of 1.07e-5°, 1.2 m |
| 11–13 | longitude | signed, 2^23 = 180°: steps of 2.15e-5°, 2.4 m at the equator |
| 14 | speed | km/h, 0–254; 255 unknown |
| 15 | course | 2° steps from north, 0–179; 255 unknown |
| 16 | status | bit 0 needs help, bit 1 stopped, bit 2 GPS ok, bit 3 battery low |

Coordinates round to the nearest step, half away from zero, and +90° and +180° clamp to the
largest code.

**Ages are honest.** A receiver unwraps the fix time to the nearest second of its own UTC, within
half the range, 9 h 6 min (RFC 1982). A fix that then lies in the future is shown with a negative
age, never as fresh. A receiver with no time of its own shows the age as unknown.

**What a receiver keeps.** Only version 0 positions of 17 bytes from its own group. A packet from
its own slot is another radio misconfigured onto it, and is counted rather than taken. Per slot it
counts the packets taken against the run of counters seen: their ratio is the delivered share of
[07](07-experiments.md). A counter that goes back starts a new run, as the sender restarted.

**Test vector.** Slot 3 of group 7, counter 0x012345, a fix at 2026-10-22 12:00:05 UTC (64 197),
47.141° N 9.5215° E, 40 km/h, heading 270°, GPS ok:

```
00 07 03 01 23 45 FA C5 43 0B 84 06 C5 56 28 87 04
```

## Lines on the console

Each board prints what it does, one line each, as `key=value` tokens: the words the phone API
takes over in M4 (ARCHITECTURE, D9).

```
TX counter=12 len=17 ok
RIDER slot=2 lat=47.1500051 lon=9.5100064 fix_age=3 heard=0.4 speed=33 course=20 gps=1 rssi=-71 snr=8.25 rx=13/14
GNSS fix=1 time=1 sats=9 hdop=0.9 bad=0
STATUS slot=1 group=1 fix=1 lat=… lon=… sats=9 hdop=0.9 time=1 tx=12 skipped=0 duty=0.43% riders=2 bad=0 foreign=0 clash=0
```

Commands: `status`, `riders`, `slot N`, `group N`, `interval MS`, `power 0–3` (0 the strongest),
`reboot`, and on the L1 Pro `bootloader`, which opens its UF2 drive for a new image.

## What v1 changes

- Authenticated encryption, with a group tag in clear (M5; T-nonce, T-tag, T-length).
- Slots by GPS time instead of random access (M6; T-timing, T-join, T-fix-time).
- The mode chosen for the group's size and freshness, from the invitation (M5; T-phy).
