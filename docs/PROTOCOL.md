# Protocol

**v1, 2026-10-22.** Positions go encrypted and authenticated under the group's key. Every part of
it may change, together with the firmware and app that use it (D3); v0, the same positions in
clear, is gone. The code is `firmware/src/phy.c`, `crypto.c`, `codec.c` and `node.c`, and the
app's `Invitation.kt`.

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

A 24-byte packet is on the air for 30.848 ms, by the same formula as `tools/airtime.py`; 25 and 26
bytes would cost no more.

The sync word keeps clear of its neighbours. Public LoRaWAN sends sync symbols 24 and 32, the
private default 8 and 16, Meshtastic 16 and 88, EBYTE's modules 40 and 36 (dmd2-lora-bridge,
PHY-notes). Whether other chips leave it alone is still to check on hardware (BACKLOG T-phy).

## When a board sends

- **Only with a key.** A board without a group key sends nothing and takes nothing: fresh from
  flashing, it is silent until a phone sets it up.
- **Random access.** A board sends its position every interval, 2 s by default, drawn anew each
  time between 0.8 and 1.2 of it, so two boards that start in step drift apart. The first packet
  goes anywhere within one interval of starting.
- **Only what it knows.** Nothing before its GNSS has given a position and a time. After that, if
  the fix is lost, the last good position goes on with its own fix time, and the GPS-ok bit clear,
  so its age keeps growing on every map.
- **Within the duty cycle.** The board books each packet's time on air against the last hour, a
  minute at a time, and skips a packet that would take it past the sub-band's duty cycle (10 %).

## The position packet: 24 bytes

| Bytes | Field | Encoding |
|---|---|---|
| 0 | version, type, flags | bits 7–6 version (1), bits 5–4 type (0, position), bits 3–0 flags (0) |
| 1 | group tag | from the group key, below |
| 2 | slot | the rider, 0–31 |
| 3–6 | session | drawn at random at every start; big-endian |
| 7–8 | counter | per session, from 0; big-endian |
| 9–10 | fix time | UTC seconds of the fix, modulo 65 536; big-endian |
| 11–13 | latitude | signed, two's complement, 2^23 = 90°: steps of 1.07e-5°, 1.2 m |
| 14–16 | longitude | signed, 2^23 = 180°: steps of 2.15e-5°, 2.4 m at the equator |
| 17 | speed | km/h, 0–254; 255 unknown |
| 18 | course | 2° steps from north, 0–179; 255 unknown |
| 19 | status | bit 0 needs help, bit 1 stopped, bit 2 GPS ok, bit 3 battery low |
| 20–23 | tag | CCM's |

Bytes 0–8 go in clear and are authenticated; bytes 9–19 are encrypted. Coordinates round to the
nearest step, half away from zero, and +90° and +180° clamp to the largest code.

**The cipher.** AES-128-CCM (RFC 3610, NIST SP 800-38C), with a 13-byte nonce, a 4-byte tag, and
bytes 0–8 as associated data. The nonce is bytes 2–8, the slot, session and counter, then six zero
bytes. A board draws a new session at every start, and again before its counter wraps, so a nonce
repeats only if two sessions of one slot draw the same 32 bits (BACKLOG T-nonce). The 4-byte tag
lets a forgery through once in 2^32 tries: a transmitter sending nothing else, at 30.8 ms a try,
needs four years on average, and what it lands is a position of random bytes (T-tag).

**From the group key.** The 16-byte key K encrypts two blocks and nothing else:

- the CCM key, AES_K(`FE` "convoy-ccm" `00`…);
- the group tag, the first byte of AES_K(`FF` "convoy-tag" `00`…).

A receiver drops a packet with another tag before any cryptography, as another group's; two groups
share a tag once in 256, and then the CCM tag tells them apart. It drops a packet whose CCM tag
does not check as forged.

**Ages are honest.** A receiver unwraps the fix time to the nearest second of its own UTC, within
half the range, 9 h 6 min (RFC 1982). A fix that then lies in the future is shown with a negative
age, never as fresh. A receiver with no time of its own shows the age as unknown.

**What a receiver keeps.** Only v1 positions of 24 bytes, with its group's tag and a CCM tag that
checks. A packet from its own slot is another radio misconfigured onto it, and is counted rather
than taken. Per slot, within a session the counter only grows; a session it has not seen must
bring a fix no more than 2 s older than the one it holds, or it is an old packet played again.
After its own restart a receiver holds nothing, so it takes the first packet of any session
([06](06-own-protocol.md), security rules). Each refusal is counted: foreign, forged, replay,
clash. Per slot it counts the packets taken against the run of counters seen in the session: their
ratio is the delivered share of [07](07-experiments.md).

**Test vector.** Under the key `00 01 … 0f` (tag `86`): slot 3, session `0x01020304`, counter 5,
a fix at 2026-10-22 12:00:05 UTC (64 197), 47.141° N 9.5215° E, 40 km/h, heading 270°, GPS ok.
From an independent implementation on OpenSSL:

```
40 86 03 01 02 03 04 00 05 EE 37 9D 2B 30 A8 88 6B 0E 6D 72 6C 62 59 B0
```

## The invitation

One string per rider. The organiser's app makes them for a new group, with a fresh key, and the
organiser sends each rider theirs; the rider's app keeps it and sets up the board from it. `LC1.`
and the base64url, without padding, of:

| Bytes | Field |
|---|---|
| 0 | version, 1 |
| 1–16 | the group key |
| 17 | the rider's slot |
| 18 | riders in the group, 1–32 |
| 19–20 | the interval in ms, 500–60 000; big-endian |
| 21 | the length of the name, up to 32 |
| 22– | the group's name, UTF-8 |

An app refuses anything else whole: another prefix or version, a slot outside the group, bytes
missing or left over. The invitation carries the key, so whoever holds it can read the group's
positions and send as any of its riders: it goes to one rider, and a new ride gets a new key
([06](06-own-protocol.md), security rules).

The key `00 01 … 0f`, slot 2 of 5, every 2 s, "Alps ride":
`LC1.AQABAgMEBQYHCAkKCwwNDg8CBQfQCUFscHMgcmlkZQ`

The app sends it to the board as `KEY`, `SLOT` and `INTERVAL` once per connection, and again when
the phone joins another group; a board keeps its riders when the key is the one it has.

## The phone API

Text lines of `key=value` tokens after a word, ending in a newline (ARCHITECTURE, D9). The same
on three transports:

| Transport | Where | Details |
|---|---|---|
| Bluetooth | a board and the app | Nordic UART Service (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`, commands to `…0002`, lines from `…0003`). The board advertises as `LoRaConvoy-` and eight hex digits of its address. Commands go only over an encrypted link: the first phone to pair (Just Works) owns the board and others are refused, until `FORGET` or the board's button held 3 s frees it (dmd2-lora-bridge's model) |
| TCP | a simulated board and the app | `make -C firmware sim ARGS="serve 3"` serves board *i* on 127.0.0.1:4403+*i*, without a key, like a board fresh from flashing; an Android emulator reaches it at 10.0.2.2 |
| USB console | a board and a person | the same commands, typed; `FORGET` there is a hand on the board, like the button |

**Version.** `proto=` in the status line: 2 since v1, which brought the key. An app refuses a board
with another (D3).

| Phone to board | Board to phone |
|---|---|
| `STATUS` | `STATUS proto=2 board=… reset=… up=… slot=… keyed=… group=… fix=… … power=… dbm=…` |
| `RIDERS` | a `RIDER` line for each rider known, then `RIDERS <n>` |
| `KEY <32 hex digits>` | `OK keyed group=<tag>`; riders heard under another key are forgotten |
| `SLOT <0–31>`, `INTERVAL <500–60000>`, `POWER <0–3>` | `OK <key>=<value>`, or `ERR <why>` |
| `FORGET` | `FORGOTTEN` once the board has dropped its bond, or `ERR forget incomplete` |
| `REBOOT`, and `BOOTLOADER` on the L1 Pro (its UF2 drive) | — |

The board keeps its key, slot and interval across restarts. No command reads the key back: the
status shows `keyed=` and the group's tag in hex, `00` without a key, and counts the packets
refused as `foreign=`, `forged=`, `replay=` and `clash=`.

Unasked, the board sends a `RIDER` line for every packet it takes, and its status every 10 s while
a phone is connected. The app sends `STATUS` every 10 s, which keeps the link from being dropped
as silent. Commands are case-blind.

```
RIDER slot=2 lat=47.1500051 lon=9.5100064 fix_age=3 heard=0.4 speed=33 course=20 gps=1 help=0 stopped=0 rssi=-71 snr=8.25 rx=13/14
```

`fix_age` is in whole seconds and can be negative (the sender's clock is wrong) or `?` (this board
has no time of its own); `heard` is in tenths of a second; `rx` is packets taken against the run of
counters. On the console the board also prints `TX`, `RX dropped` and `GNSS` lines. Slots count
from 0 here; the app shows riders from 1.

## What comes next

- Slots by GPS time instead of random access (M6; T-timing, T-join, T-fix-time), and with them
  perhaps the frame number in the nonce instead of the session (T-nonce).
- The mode chosen for the group's size and freshness, carried in the invitation (T-phy).
- A shorter packet: an implicit header, the CRC left to CCM (T-length).
