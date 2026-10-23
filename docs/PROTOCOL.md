# Protocol

**v2, 2026-10-23.** Positions go encrypted and authenticated under the group's key (M5), each
rider in a slot of its own, timed by GPS (M6), and each packet answers a call for help it hears
(M7). Every part of it may change, together with the firmware and app that use it (D3): v0 had
positions in clear, v1 no answers to calls for help, and boards with either are reflashed. The code is
`firmware/src/phy.c`, `crypto.c`, `codec.c` and `node.c`, and the app's `Invitation.kt`.

## Radio mode

| | v1, as v0 |
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

A 25-byte packet is on the air for 30.848 ms, by the same formula as `tools/airtime.py`: the same
as 24 bytes, and 26 would cost no more.

The sync word keeps clear of its neighbours. Public LoRaWAN sends sync symbols 24 and 32, the
private default 8 and 16, Meshtastic 16 and 88, EBYTE's modules 40 and 36 (dmd2-lora-bridge,
PHY-notes). Whether other chips leave it alone is still to check on hardware (BACKLOG T-phy).

## When a board sends

Once a frame, in its own slot, by GPS time ([06](06-own-protocol.md), Timing).

```
frame k: the interval (2 s), n riders, a spacing of interval / (n + 1); here n = 3, 500 ms

k x 2 s   + phase   slot 0            slot 1            slot 2            spare
|---------|=====....|-----------------|=====....|-------|=====....|-------|---------|
          <-- 500 ms --><-- 500 ms --><-- 500 ms --><-- 500 ms -->   frame k+1, its own phase
```

- **Only with a key.** A board without a group key sends nothing and takes nothing: fresh from
  flashing, it is silent until a phone sets it up.
- **Frames by GPS time.** Frames are numbered from 1970 in UTC: frame k starts at k intervals, 2 s
  by default, from the invitation. A frame holds a slot for each of the group's n riders and one
  spare, evenly spaced: interval / (n + 1) apart. Rider s sends at the frame's start, plus the
  frame's phase, plus s spacings. Nobody in a group sends over anybody else in it, however large it
  is.
- **A phase for each frame.** The phase is under one spacing, the same for the whole group, and
  drawn anew for every frame from the group's key: the first four bytes of AES, under a key the
  group key gives (`FD` "convoy-frame"), of the frame's number, modulo the spacing. Two groups on
  one channel then meet in some frames, never in all of them; the spare slot keeps the last packet
  of one frame a spacing clear of the first of the next, whatever their phases.
- **Time.** A board's clock is set by marks. With a PPS line (the Heltec's kit), the edge is the
  second that the next NMEA fix names, within 100 µs. Without one (the L1), the fix's arrival less
  the GNSS's lag, as far off as the board is told: 100 ms until E0 has measured the L76K. A mark is
  taken to drift 50 ppm as it ages, and a new one replaces it only when better.
- **A budget for the time.** A board sends only while its time, plus 1 ms for waking and the
  radio's start, is within half the gap between two packets: the spacing, less the packet and 2 ms
  for receivers to read it out. Otherwise it passes the slot over, and counts it (`unsure=`); a
  slot whose moment it missed by over 2 ms is passed over too (`late=`). Every 2 s:

  | Riders | Spacing | Budget | A board with PPS keeps its slot without GNSS for | A board without PPS, its lag unmeasured (±100 ms) |
  |---|---|---|---|---|
  | 3 | 500 ms | 233 ms | 77 min | sends; keeps its slot 44 min without GNSS |
  | 7 | 250 ms | 108 ms | 36 min | sends; 2.5 min |
  | 10 | 182 ms | 74 ms | 24 min | silent |
  | 20 | 95 ms | 31 ms | 10 min | silent |
  | 32 | 61 ms | 14 ms | 4 min | silent |

- **Listening in its own slot.** Once in 32 frames at random a board listens in its slot instead
  of sending. Two radios given one slot, as when an invitation is used twice, meet in every frame,
  so nobody hears either, and neither hears the other while both send; listening, they hear each
  other within half a minute on average, counted as `clash=`, and the app says so. It costs 3 % of
  the positions.
- **Only what it knows.** Nothing before its GNSS has given a position and a time. After that, if
  the fix is lost, the last good position goes on with its own fix time, and the GPS-ok bit clear,
  so its age keeps growing on every map.
- **Within the duty cycle.** The board books each packet's time on air against the last hour, a
  minute at a time, and skips a packet that would take it past the sub-band's duty cycle (10 %).
  At one packet a frame it cannot get there: 30.8 ms in the shortest frame, 500 ms, is 6.2 %.

## The position packet: 25 bytes

| Bytes | Field | Encoding |
|---|---|---|
| 0 | version, type, flags | bits 7–6 version (2), bits 5–4 type (0, position), bits 3–0 flags (0) |
| 1 | group tag | from the group key, below |
| 2 | slot | the rider, 0–31 |
| 3–6 | session | drawn at random at every start; big-endian |
| 7–8 | counter | per session, from 0; big-endian |
| 9–10 | fix time | UTC seconds of the fix, modulo 65 536; big-endian |
| 11–13 | latitude | signed, two's complement, 2^23 = 90°: steps of 1.07e-5°, 1.2 m |
| 14–16 | longitude | signed, 2^23 = 180°: steps of 2.15e-5°, 2.4 m at the equator |
| 17 | speed | km/h, 0–254; 255 unknown |
| 18 | course | 2° steps from north, 0–179; 255 unknown |
| 19 | status | bit 0 needs help, bit 1 stopped, bit 2 GPS ok, bit 3 battery low (not yet set) |
| 20 | help heard | the slot of a rider whose call for help the sender hears; 255 for none |
| 21–24 | tag | CCM's |

Bytes 0–8 go in clear and are authenticated; bytes 9–20 are encrypted. Coordinates round to the
nearest step, half away from zero, and +90° and +180° clamp to the largest code.

**The cipher.** AES-128-CCM (RFC 3610, NIST SP 800-38C), with a 13-byte nonce, a 4-byte tag, and
bytes 0–8 as associated data. The nonce is bytes 2–8, the slot, session and counter, then six zero
bytes. A board draws a new session at every start, and again before its counter wraps, so a nonce
repeats only if two sessions of one slot draw the same 32 bits (BACKLOG T-nonce). The 4-byte tag
lets a forgery through once in 2^32 tries: a transmitter sending nothing else, at 30.8 ms a try,
needs four years on average, and what it lands is a position of random bytes (T-tag).

**From the group key.** The 16-byte key K encrypts three blocks and nothing else:

- the CCM key, AES_K(`FE` "convoy-ccm" `00`…);
- the group tag, the first byte of AES_K(`FF` "convoy-tag" `00`…);
- the key that draws each frame's phase, AES_K(`FD` "convoy-frame" `00`…) (When a board sends).

A receiver drops a packet with another tag before any cryptography, as another group's; two groups
share a tag once in 256, and then the CCM tag tells them apart. It drops a packet whose CCM tag
does not check as forged.

**Ages are honest.** A receiver unwraps the fix time to the nearest second of its own UTC, within
half the range, 9 h 6 min (RFC 1982). A fix that then lies in the future is shown with a negative
age, never as fresh. A receiver with no time of its own shows the age as unknown.

**What a receiver keeps.** Only v2 positions of 25 bytes, with its group's tag and a CCM tag that
checks. A packet from its own slot is another radio misconfigured onto it, and is counted rather
than taken. Per slot, within a session the counter only grows; a session it has not seen must
bring a fix no more than 2 s older than the one it holds, or it is an old packet played again.
After its own restart a receiver holds nothing, so it takes the first packet of any session
([06](06-own-protocol.md), security rules). Each refusal is counted: foreign, forged, replay,
clash. Per slot it counts the packets taken against the run of counters seen in the session: their
ratio is the delivered share of [07](07-experiments.md).

**Help, answered.** A rider's call for help sets the help bit in each of their packets. A board
that hears a call answers it in its own next packet, in byte 20: the slot of a rider it has heard
calling within the last minute, each in turn when several call. The calling board counts the
riders who answer (`heard=` in its status), and every board reports whom each rider answers
(`acks=` in its RIDER lines), so every map can show who has heard a call. In the simulator, among
twenty riders with 10 % of packets lost, the first answer comes 1.1 s after the call and the
last of the nineteen 5.4 s after it, at the median; 3.5 s and 10.4 s at worst.

**Stopped.** The board sets the stopped bit after a minute under 5 km/h, so a red light is not a
stop, and clears it over 10 km/h, so a GNSS wandering at a standstill does not start it.

**Test vector.** Under the key `00 01 … 0f` (tag `86`): slot 3, session `0x01020304`, counter 5,
a fix at 2026-10-22 12:00:05 UTC (64 197), 47.141° N 9.5215° E, 40 km/h, heading 270°, GPS ok,
hearing slot 7's call for help. From an independent implementation on OpenSSL (pyca/cryptography),
which also gives v1's vector:

```
80 86 03 01 02 03 04 00 05 EE 37 9D 2B 30 A8 88 6B 0E 6D 72 4D 87 79 6E 2E
```

## The invitation

One string per rider. The organiser's app makes them for a new group from the riders' names, the
organiser's first, with a fresh key, and the organiser sends each rider theirs; the rider's app
keeps it, sets up the board from it, and names the riders on the map by it. `LC2.` and the
base64url, without padding, of:

| Bytes | Field |
|---|---|
| 0 | version, 2 |
| 1–16 | the group key |
| 17 | the rider's slot |
| 18 | riders in the group, 1–32 |
| 19–20 | the interval in ms, 500–60 000; big-endian |
| 21– | the group's name, up to 32 bytes; then each rider's name in slot order, up to 16 bytes, empty for "Rider N". Each is its length in a byte, then its UTF-8 |

An app refuses anything else whole: another prefix or version, a slot outside the group, a name
missing, bytes left over. v1 (`LC1.`) had no riders' names. The invitation carries the key, so whoever holds it can read the group's
positions and send as any of its riders: it goes to one rider, and a new ride gets a new key
([06](06-own-protocol.md), security rules).

The key `00 01 … 0f`, slot 2 of 5, every 2 s, "Alps ride", riders Ann, Bob, Cid, Dee and Eve:
`LC2.AgABAgMEBQYHCAkKCwwNDg8CBQfQCUFscHMgcmlkZQNBbm4DQm9iA0NpZANEZWUDRXZl`

The app sends it to the board as `KEY`, `SLOTS`, `SLOT` and `INTERVAL` once per connection, and
again when the phone joins another group; a board keeps its riders when the key is the one it has.

## The phone API

Text lines of `key=value` tokens after a word, ending in a newline (ARCHITECTURE, D9). The same
on three transports:

| Transport | Where | Details |
|---|---|---|
| Bluetooth | a board and the app | Nordic UART Service (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`, commands to `…0002`, lines from `…0003`). The board advertises as `LoRaConvoy-` and eight hex digits of its address. Commands go only over an encrypted link: the first phone to pair (Just Works) owns the board and others are refused, until `FORGET` or the board's button held 3 s frees it (dmd2-lora-bridge's model) |
| TCP | a simulated board and the app | `make -C firmware sim ARGS="serve 3"` serves board *i* on 127.0.0.1:4403+*i*, without a key, like a board fresh from flashing; an Android emulator reaches it at 10.0.2.2 |
| USB console | a board and a person | the same commands, typed; `FORGET` there is a hand on the board, like the button |

**Version.** `proto=` in the status line: 4 since help. An app refuses a board with another (D3).

| Phone to board | Board to phone |
|---|---|
| `STATUS` | `STATUS proto=4 board=… reset=… up=… slot=… slots=… keyed=… group=… fix=… stopped=… help=… heard=… … power=… dbm=…` |
| `RIDERS` | a `RIDER` line for each rider known, then `RIDERS <n>` |
| `LEDGER` | `LEDGER minutes=60 limit_ms=… used_ms=… positions_ms=…`: the duty ledger, transmit time a minute at a time over the last hour, oldest first, by category |
| `KEY <32 hex digits>` | `OK keyed group=<tag>`; riders heard under another key are forgotten |
| `HELP <0–1>` | `OK help=<0–1>`: our rider's call for help, off or on; each time on, who answers starts afresh |
| `SLOT <0–31>`, `SLOTS <1–32>`, `INTERVAL <500–60000>`, `POWER <0–3>` | `OK <key>=<value>`, or `ERR <why>` |
| `MODE <SF 7–12> <kHz 62, 125, 250, 500>` | `OK mode=<SF>/<kHz>`: another radio mode on the same frequency, for trying range (E2); kept over a restart. Every board of a group needs the same |
| `FORGET` | `FORGOTTEN` once the board has dropped its bond, or `ERR forget incomplete` |
| `REBOOT`, and `BOOTLOADER` on the L1 Pro (its UF2 drive) | — |

The board keeps its key, slots, interval and mode across restarts. No command reads the key back: the
status shows `keyed=` and the group's tag in hex, `00` without a key, and counts the packets
refused as `foreign=`, `forged=`, `replay=` and `clash=`. Of its time it shows where it comes from
(`time=none`, `nmea` or `pps`), how far off it may be and the budget, in ms (`err=`, `budget=`),
and the slots passed over (`late=`, `unsure=`). A board with both PPS and NMEA measures E0's number
for boards without PPS: `lag=` is the NMEA fix's lag behind its PPS edge, last, least and most, in
ms.

Unasked, the board sends a `RIDER` line for every packet it takes, and its status every 10 s while
a phone is connected. The app sends `STATUS` every 10 s, which keeps the link from being dropped
as silent. Commands are case-blind.

```
RIDER slot=2 lat=47.1500051 lon=9.5100064 fix_age=3 heard=0.4 speed=33 course=20 gps=1 help=0 stopped=0 acks=- rssi=-71 snr=8.25 rx=13/14
```

`fix_age` is in whole seconds and can be negative (the sender's clock is wrong) or `?` (this board
has no time of its own); `heard` is in tenths of a second; `acks` is the slot whose call for help
the rider's board answers, `-` for none; `rx` is packets taken against the run of counters. On the console the board also prints `TX`, `RX dropped` and `GNSS` lines. Slots count
from 0 here; the app shows riders from 1.

## What comes next

- The frame number in the nonce instead of the session (T-nonce), and the fix's age at the slot
  instead of its second (T-fix-time).
- Time for boards without a fix, from the packets they hear, and a clock disciplined against PPS
  for longer holdover (T-timing); the L76K's lag, measured (E0).
- The mode chosen for the group's size and freshness, carried in the invitation (T-phy).
- A shorter packet: an implicit header, the CRC left to CCM (T-length).
