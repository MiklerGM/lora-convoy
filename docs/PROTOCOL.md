# Protocol

**v5, 2026-11-14.** Each rider in a slot of its own, timed by GPS (M6), sending when it has
something to say: a move, a turn, a change, or a heartbeat; each packet encrypted and
authenticated under the group's key, carrying the rider's own record and short records of other
riders it has heard of (passengers), so a column reaches past its radios' range. A board without
PPS sets its clock by the packets of boards with it. Decided with the owner on 2026-11-14
([12](12-protocol-v5-proposal.md), what the owner's review changed). Every part of it may change,
together with the firmware and app that use it (D3): v3 sent a position every frame and answered
calls for help packet by packet; boards with it are reflashed. The code is `firmware/src/phy.c`,
`crypto.c`, `codec.c` and `node.c`, and the app's `Invitation.kt`.

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
| Power | `POWER 1` by default: 24 dBm on the Heltec V4.3, 20 dBm on the L1 Pro and the T-Echo; `POWER 0` is 28 and 22 dBm, `POWER 3` is 14 dBm on all. Kept over a restart with the group, from 2026-11-16 |

A 25-byte packet is on the air for 30.848 ms, by the same formula as `tools/airtime.py`: the same
as 24 bytes, and 26 would cost no more.

The sync word keeps clear of its neighbours. Public LoRaWAN sends sync symbols 24 and 32, the
private default 8 and 16, Meshtastic 16 and 88, EBYTE's modules 40 and 36 (dmd2-lora-bridge,
PHY-notes). Whether other chips leave it alone is still to check on hardware (BACKLOG T-phy).

## When a board sends

In its own slot, by GPS time ([06](06-own-protocol.md), Timing), when it has something to say.

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
- **Time.** A board's clock is set by marks. With a PPS line (the Heltec's kit, the T-Echo), the
  edge is the second that the next NMEA fix names, within 100 µs. Without one (the L1), the fix's
  arrival less the GNSS's lag, as far off as the board's build tells it: 138 ms and 8.7 ms for
  each satellite the fix used, give or take 100 ms ([07](07-experiments.md): ±50 ms held at a
  window for days and failed on a balcony for an afternoon). The L76K is told to send GGA and RMC
  only (`$PCAS03`). A mark is taken to drift 50 ppm as it ages, 150 on the T-Echo, whose clock
  ran 102–115 ppm off ([07](07-experiments.md)), and a new one replaces it only when better.
- **Time from the group.** A board on PPS whose mark is within 5 ms says so in each packet
  ("time precise"). A board that hears such a packet knows when that slot should have begun by its
  own clock, and when it did: the median of the latest five such differences sets its clock, give
  or take 10 ms (`time=group`), where that is better than what it has. It always sends by its own
  clock, never on hearing a packet, so a lost packet costs nothing; its bound grows by drift until
  the next one. It never says "precise" itself, so boards do not set each other in a circle. On the
  desk, 2026-11-14, an L1 so set kept to its slot within 0.6 ms on the median, sd 0.8 ms, over
  14,333 packets in 8 hours, judged by the Heltec on PPS; on its NMEA it had stood at −20 ms, sd 8.
- **A budget for the time.** A board sends only while its time, plus 1 ms for waking and the
  radio's start, is within half the gap between two packets: the spacing, less the packet and 2 ms
  for receivers to read it out. Otherwise it passes the slot over, and counts it (`unsure=`); a
  slot whose moment it missed by over 2 ms is passed over too (`late=`). The packet is handed to
  the radio its start-up before the slot, so its RF begins on the slot's moment: 8.0 ms on the
  Heltec, 7.1 ms on the L1, measured, and steady to 0.2 ms. Every 2 s:

  | Riders | Spacing | Budget | A board with PPS keeps its slot without GNSS for | Without PPS, on its NMEA (±100 ms) | Without PPS, set by the group (±10 ms), without hearing a PPS board for |
  |---|---|---|---|---|---|
  | 3 | 500 ms | 230 ms | 76 min | sends | 73 min |
  | 7 | 250 ms | 105 ms | 35 min | sends | 31 min |
  | 10 | 182 ms | 71 ms | 23 min | silent | 20 min |
  | 13 | 143 ms | 51 ms | 17 min | silent | 13 min |
  | 20 | 95 ms | 27 ms | 9 min | silent | 5 min |
  | 32 | 61 ms | 10 ms | 3 min | silent | silent |

  For a 34-byte packet (one passenger) at SF7/250, 38.5 ms; the slot is sized for the most
  passengers the group carries.
- **Listening in its own slot.** Once in 32 frames at random a board listens in its slot instead
  of sending. Two radios given one slot, as when an invitation is used twice, meet in every frame,
  so nobody hears either, and neither hears the other while both send; listening, they hear each
  other within half a minute on average, counted as `clash=`, and the app says so. It costs 3 % of
  the positions.
- **When it speaks** (the send rule; numbers per group, `GROUP` and the bench's `BEAT`, `MOVE`):
  in its slot it sends if it has moved 10 m from the position it last sent, or turned 30° once 5 m
  on, or its state, stopped or current-fix fact changed (then in its next three slots, so that one
  loss does not hide it), or another rider it carries has changed state (three slots), or it has
  been silent for 2^beat frames: 8 by default, 16 s at 2 s. Otherwise it keeps the slot quiet, and
  counts it (`quiet=`). The rider's GNSS wanders 3–5 m at a standstill, hence 10 m.
- **Only what it knows.** Nothing before its GNSS has given a time. Before a first fix it says
  "no position", with its state: a rider in trouble is heard before its fix. After a fix is lost
  the last good position goes on with its true age and the "current" fact clear.
- **Within the duty cycle.** The board books each packet's time on air against the last hour, a
  minute at a time, and skips a packet that would take it past the sub-band's duty cycle (10 %).
  At one packet a frame it cannot get there: 38.5 ms in the shortest frame, 500 ms, is 7.7 %.

## The packet: 25 bytes, and 9 for each rider carried

| Bytes | Field | Encoding |
|---|---|---|
| 0 | version, type | bits 7–4 version (5), bits 3–0 type (0, status) |
| 1 | group tag | from the group key, below |
| 2 | slot | the sender, 0–31 in bits 4–0; bits 7–5 zero |
| 3–4 | session | drawn at random at every start, and again if the clock steps back; big-endian |
| 5–8 | sent | UTC seconds of the slot it is sent in; big-endian |
| 9 | count | packets of this session, modulo 256 |
| 10 | age | of the fix when sent: seconds to 119, then 118 + minutes to two hours (120–239), 254 older, 255 no position |
| 11–13 | latitude | signed, two's complement, 2^23 = 90°: steps of 1.07e-5°, 1.2 m |
| 14–16 | longitude | signed, 2^23 = 180°: steps of 2.15e-5°, 2.4 m at the equator |
| 17 | speed | km/h, 0–254; 255 unknown |
| 18 | course | 2° steps from north, 0–179; 255 unknown |
| 19 | facts, state | bits 3–0 facts: 0 stopped, 1 battery low (not yet set), 2 time precise, 3 the fix is current; bits 7–4 the state its rider chose: 0 normal, 1 need help, 2 stop now, 3 a stop wanted when convenient (fuel, a snack, a rest), 4 helping; the rest free |
| 20 | heartbeat, satellites | bits 2–0: it sends at least every 2^h frames; bits 6–3 the satellites its fix used, to 15, 0 with no current fix; bit 7 zero |
| then, 9 each | passengers | slot (bits 4–0) and "has a position" (bit 5); latitude, longitude as above; how long ago it said so itself, as the age above; facts and state as byte 19 |
| last 4 | tag | CCM's |

Bytes 0–9 go in clear and are authenticated; the rest is encrypted. A packet carries none to three
passengers, by its length. Coordinates round to the nearest step, half away from zero, and +90°
and +180° clamp to the largest code.

**The cipher.** AES-128-CCM (RFC 3610, NIST SP 800-38C), with a 13-byte nonce, a 4-byte tag, and
bytes 0–9 as associated data. The nonce is bytes 2–9, the slot, session, second and count, then
five zero bytes. A board never sends twice in one (second, count), and draws a new session when its
clock steps back, so a nonce repeats only for two radios misconfigured onto one slot that draw the
same 16-bit session and send in the same second with the same count. Protection is a shared
password's, no more (owner, 2026-11-14): other groups kept off the map, positions unreadable to a
passer-by, a recording refused; no claim against a determined attacker. The 4-byte tag lets a
forgery through once in 2^32 tries.

**From the group key.** The 16-byte key K encrypts three blocks and nothing else:

- the CCM key, AES_K(`FE` "convoy-ccm" `00`…);
- the group tag, the first byte of AES_K(`FF` "convoy-tag" `00`…);
- the key that draws each frame's phase, AES_K(`FD` "convoy-frame" `00`…) (When a board sends).

A receiver drops a packet with another tag before any cryptography, as another group's; two groups
share a tag once in 256, and then the CCM tag tells them apart. It drops a packet whose CCM tag
does not check as forged.

**What a receiver keeps.** A packet of its group whose CCM tag checks, sent within 3 s of its own
time where it has one, and later than the last it took from that rider by (second, count). An old
packet played back is refused by a receiver with a time of its own; one just restarted, with no
GNSS time yet and its riders forgotten, takes the first packet of each rider as it comes, its age
as that packet tells it, unchecked until the receiver has a time. A packet from its own slot is
another radio misconfigured onto it, and is counted rather than taken. Each refusal is counted:
foreign, forged, replay, clash. Per rider it counts the packets taken against those its count says
were sent: the delivered share of [07](07-experiments.md).

**Passengers.** Each rider is held as the latest record of it, from itself or carried by another:
where it was, its state and facts, and the second it said so itself. A sender carries, first, a
rider whose state has just changed (in its next three packets), then whoever it told longest ago,
never one it last heard of over 10 minutes before. A receiver takes a carried record only if it is
later than the one it holds, by more than the age's own step: a record never gets younger on its
way, nothing loops and nothing floods, and the latest state of a rider travels down a column past
its radios' reach. A sender seeing its own record carried by another, in the state it is in now and
surely no older than its first packet in that state (a record of an earlier call for help, before an all
well, is not of this one; surely, as an age from two minutes on goes in whole minutes rounded down,
so it is read as up to 59 s more, and one beyond two hours never counts), knows its packets got out (`heard=`, `echo=` in its status): the confirmation v3's answers gave, at
no cost in air. In the simulator, a column of 20 spreading to 9.5 km with radios reaching 1 km:
a call for help reaches all 19 within 10 s; riders out of reach are 26 s old at the median with one
passenger, 12 s with two, 8 s with three.

**Stopped.** The board sets the stopped fact after a minute under 5 km/h and within 30 m of where
that minute began, so a red light is not a stop and a walker at 4 km/h is not stopped; it clears it
over 10 km/h or 30 m from where the minute began, so a climb at 7 km/h after a stop starts it, and
a GNSS wandering a few metres at a standstill does not.

**Test vector.** Under the key `00 01 … 0f` (tag `86`): slot 3, session `0x0102`, sent at
2026-10-22 12:00:05 UTC (1 792 670 405), count 5, a current fix 2 s old on PPS time,
47.141° N 9.5215° E, 40 km/h, heading 270°, heartbeat 3, 12 satellites, calling for help; one
passenger, slot 7 at 47.15° N 9.51° E, stopped and wanting a stop, as it said 12 minutes before.
From an independent implementation on OpenSSL (pyca/cryptography):

```
50 86 03 01 02 6A D9 FA C5 05 60 E6 68 00 30 F3 70 89 EF BB 87 3F F3 08 40 9C C5 4E C1 8C 76 99 1C 4B
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

The app sends it to the board as one `GROUP` once per connection, and again when the phone joins
another group; a board takes it whole or not at all, and keeps its riders when the key is the one
it has. Once the board says it took the group, the app asks for its riders. Each `RIDER` line names
the group it was heard in (`group=`), and the app keeps only the riders of the group the board says
it is in: those of the group before go at once, whatever lines were lost on the way, a slot of
theirs taken again or not, and a late line of theirs is not taken. A rider the board no longer
hears stays, ageing. The app keeps the group it wants, and shows the board in it only once the
board has said so, never by the tag, which two keys can share.

## The phone API

Text lines of `key=value` tokens after a word, ending in a newline (ARCHITECTURE, D9). The same
on three transports:

| Transport | Where | Details |
|---|---|---|
| Bluetooth | a board and the app | Nordic UART Service (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`, commands to `…0002`, lines from `…0003`). The board advertises as `LoRaConvoy-` and eight hex digits of its address. Commands go only over an encrypted link, and lines come back only over one, since they carry the riders' positions in clear; the L1 takes even the subscription only encrypted. The first phone to pair (Just Works) owns the board and others are refused, until `FORGET` or the board's button held 3 s frees it (dmd2-lora-bridge's model) |
| TCP | a simulated board and the app | `make -C firmware sim ARGS="serve 3"` serves board *i* on 127.0.0.1:4403+*i*, without a key, like a board fresh from flashing; an Android emulator reaches it at 10.0.2.2 |
| USB console | a board and a person | the same commands, typed; `FORGET` there is a hand on the board, like the button |

**Version.** `proto=` in the status line: 8 since v5's packet (states, passengers, the send rule). An app refuses a board with another (D3).

| Phone to board | Board to phone |
|---|---|
| `STATUS` | `STATUS proto=8 board=… reset=… at=… slot=… slots=… keyed=… group=… fix=… stopped=… heard=… state=… echo=… send=… … riders=… … power=… dbm=… kept=…` |
| `RIDERS` | a `RIDER` line for each rider known, then `RIDERS <n>` |
| `CLOCK <t1>` | `CLOCK t1=<t1> boot=<run> rx=<µs> tx=<µs> utc=<ms>`: the phone's moment back, with the board's clock when the request came and when it made the answer, and its GNSS time then, `?` without one, which the app tells its riders' ages by rather than its own calendar. Exchanged three times as a link begins and every 10 s after; each gives the board's clock less the phone's, ((rx − t1) + (tx − t4)) / 2, to within half its round trip, and the quickest is believed, its error growing by 100 ppm of drift as it ages |
| `TIME <ms> <utc ms> <err ms>` | `OK time=phone err=<ms>`: the phone's UTC at the board's `<ms>`, give or take `<err ms>`: a calendar for the riders' ages while the board has no GNSS time, kept apart from GNSS time and below it, and never opening a slot |
| `GROUP <32 hex digits> <slot> <slots> <interval>` | `OK group=<id> slot=… slots=… interval=…`: a group's whole setup, from the invitation, checked whole and taken whole; or `ERR group: …`, and nothing changes |
| `LEDGER` | `LEDGER minutes=60 limit_ms=… used_ms=… positions_ms=…`: the duty ledger, transmit time a minute at a time over the last hour, oldest first, by category |
| `KEY <32 hex digits>` | `OK keyed group=<id>`; riders heard under another key are forgotten |
| `STATE <0–15>` | `OK state=<n>`: the state our rider chose (the packet's byte 19); a change goes out in our next three slots, and who carries it starts afresh. `HELP <0–1>` is states 1 and 0, for an app with that button only |
| `BEAT <0–7>`, `MOVE <1–1000>`, `PASSENGERS <0–3>` | `OK <key>=<value>`: the send rule's heartbeat (every 2^n frames, 0 every frame) and distance in metres, and the riders carried in each packet; kept over a restart with the group, as `POWER` is, so a board with no phone rides as it was set (3, 10 and 1 as built). The app's Board settings sends them |
| `DEAF <3,4 or ->` | `OK deaf=<hex mask>`: for the bench, the board takes nothing directly from these slots, as if they were out of its reach, and still has them from riders who carry them: a chain on one desk. `-` hears everyone; not kept |
| `LOG <boot> <n>` | `OK log boot=<run> from=<n> next=<n>`, then the board's own console lines, those only the cable had (`TX`, `RX dropped`, `GNSS`, `WARN`, `BOOT`, and `BAT <mV> mV` as the battery moves by 50 mV), as `LOG n=<number> at=<our ms> <line>`: first those it still keeps from `<n>` on if `<boot>` is its run, else all it keeps, then each as it comes, until the link goes. It keeps the last 128, each to 79 characters; a number skipped is a line no longer kept. The app asks once per link from where its log ends, and writes everything to and from the board in a file a day (Share log) |
| `SLOT <0–31>`, `SLOTS <1–32>`, `INTERVAL <500–60000>`, `POWER <0–3>` | `OK <key>=<value>`, or `ERR <why>` |
| `MODE <SF 7–12> <kHz 62, 125, 250, 500>` | `OK mode=<SF>/<kHz>`: another radio mode on the same frequency, for trying range (E2); kept over a restart. Every board of a group needs the same |
| `TOUR <0–255>` | `OK tour=<s>`: for trying range (E2), the board changes its mode by the clock, SF7/250, SF9/250 and SF10/125 in turn, each for so many seconds of UTC, so a group's boards change together and one walk tries all three; 0 stops it, in the mode it is in. Kept over a restart |
| `ZONE <minutes>` | `OK zone=<minutes>`: the phone's offset from UTC, −720 to 840 in quarter hours, for the clock on the board's screen; kept over a restart. The app sends it on each link |
| `GNSS <0–4>` | `OK gnss=<0–4>`: for the bench. 0 and 1, the board's GNSS off or on: off, it runs on the time and the position it holds, as in a tunnel; on again at a restart. 2, 3 and 4 restart the L76K hot, warm or cold (its `PCAS10` 0, 1, 2), warm forgetting the satellites' orbits, cold all it knew of the sky, time and place |
| `SYSTEMS <1–8>` | `OK systems=<1–8>`: which satellites the L76K uses, for the bench: 1 GPS, 2 BeiDou, 4 GLONASS, summed (its `$PCAS04`; 3 as it comes; the units tried had no GLONASS, 5 being as 1 and 7 as 3). 8: 3 and 1 in turn, a quarter of an hour of UTC each, 3 from the hour and the half hour. Fewer satellites, less lag ([07](07-experiments.md)). Not kept over a restart of the board |
| `QUIET <0–2>` | `OK quiet=<0–2>`: the GNSS's quiet windows off, on, or 2 s late, for the bench; not kept, the board's own again at a restart (on for the T-Echo). With them, no send in one window a minute, in turn on GPS's subframes 1, 2 and 3 (UTC :12–:18, :18–:24, :24–:30) and GLONASS's strings 1–4 (:30–:38), from 50 ms before to 150 ms after: our frames are locked to GPS time, so a send each frame would hit the same bits of every satellite's message, and a GNSS a send deafens would never read the orbits (the T-Echo, lab 2026-11-22). What it had to say goes in the frame after; a tenth of the sends. 2 s late, as many go, but each subframe 1–3 and GLONASS's string 1 keeps its first send: a control for whether those seconds matter |
| `GSV <0–1>` | `OK gsv=<0–1>`: for the bench, the L76K adds GSV, and the board prints each sentence on its console as `GSV GP 1/3 in=11 05:32 13:- …`, each satellite's number and C/N0 in dB-Hz (`-` untracked); not in `LOG`. GSV comes before RMC, so a board on NMEA time alone dates its seconds late with it; one with PPS does not. Not kept over a restart |
| `FORGET` | `FORGOTTEN` once the board has dropped its bond, or `ERR forget incomplete` |
| `REBOOT`, and `BOOTLOADER` on the L1 Pro (its UF2 drive) | — |

The board keeps its key, slots, interval and mode across restarts; `kept=0` says it could not save
them, and would come back without them. A reply over Bluetooth stops at a line the board could
not send, never its end after a gap. `KEY`, `SLOT`, `SLOTS` and `INTERVAL` set one each, for a
person at the console. No command reads the key back: the
status shows `keyed=` and the group's id, `00000000` without a key: the first four bytes of the
block the tag comes from, so the tag first, `86c91c92` for the key `00 01 … 0f`. It counts the packets
refused as `foreign=`, `forged=`, `replay=` and `clash=`. Of its time it shows where it comes from
(`time=none`, `nmea`, `group` or `pps`), how far off it may be and the budget, in ms (`err=`, `budget=`),
the calendar the riders' ages are told by and its error in ms (`cal=pps/0`, or `cal=phone/1020`
on the phone's word alone),
and the slots passed over (`late=`, `unsure=`, `quiet=` with nothing to say, and `held=` while the radio has yet to take a mode
asked for: `mode=` is the mode asked for, and until the radio has taken it the board sends nothing,
ARCHITECTURE D15). A board with both PPS and NMEA measures E0's number
for boards without PPS: `lag=` is the NMEA fix's lag behind its PPS edge, last, least and most, in
ms. `txlead=` is how late its own packets' RF began after their slot's moment, least/mean/most in
µs, by the radio's TxDone less the airtime (E0): what is left once the packet was handed over the
radio's start-up early, near 0 on a board built with its own. `state=` is our rider's state;
`heard=` how many riders carry it on as it is now, and `echo=` seconds since one last did, `-`
before any; `send=` the heartbeat, metres and passengers; `deaf=` the slots it does not hear. Our own position is not in the status
(a line holds 511 characters): the app has it from the phone.

Unasked, the board sends a `RIDER` line for every packet it takes, and its status every 10 s while
a phone is connected, and a `RIDER` line for each rider it carried a newer record of, after
the sender's; with the packet that brings it, its own status when its own state is newly
carried. The app sends `STATUS` every 10 s, which keeps the link from being dropped
as silent. Commands are case-blind.

```
RIDER slot=2 group=86c91c92 boot=1a2b3c4d at=70100 heard_at=69700 fix=64200 pos=1 lat=47.1500051 lon=9.5100064 fix_age=3 heard=0.4 speed=33 course=20 gps=1 help=0 stopped=0 answered=- state=0 via=- direct=0.4 rssi=-71 snr=8.25 rx=13/14 offset=310/420/980 late=402 sats=9
```

`boot` is the board's run, drawn as it starts; `at` and `heard_at`, when the board made the line
and when the rider's record came, by itself or carried, on its own clock in ms since it started, which a later GNSS time
does not change the meaning of; `fix` is the rider's fix time, UTC seconds modulo 65 536: from its own
packet the second it sent less the fix's age; carried by another, the second it said so itself
while its GPS was current (`gps=1`), else `?`: a held fix passed on has no time the carrier knows,
and the second of its record would make it young; `fix_age` is then `?` too.
`pos=0` is a rider with no position yet, its `lat` and `lon` meaningless. The app tells the fix's age by that time on the board's GNSS time where the board
has it (`CLOCK`'s `utc`), else on its own calendar, either way with no wait on the way in it, and the radio contact's by `heard_at`, which it places on
its own monotonic clock by `CLOCK` exchanges, run by run: a better reckoning moves a rider kept
long since, and a board restarted is a new run, its riders of before left where they were.
`fix_age` is in whole seconds and can be negative (the sender's clock is wrong) or `?` (this board
has no calendar yet, neither GNSS time nor the phone's `TIME`); `heard` is in tenths of a second, since its record came; `direct` the same since we heard
the rider itself, `-` if never; `via` the slot that carried the record, `-` from itself;
`speed` and `course` are `?` for a carried record; `help=1` is `state=1`, and `answered=` stays
`-` (v3's, for an app that reads it); `rx` is packets taken against those its count says were sent, by an 8-bit count: across a gap of 256 packets or more it wraps, and the share comes out too high (301 sent, 2 taken, read as 2/45), so a field run's delivery is told against the sender's own log; `offset` is how late its packets began against their moment, in µs, least/mean/most, from their RxDone on this board's UTC less their airtime (E0, [BENCH](BENCH.md)), and `late`
the last of them; `sats` the satellites its fix used. On the console the board also prints `TX`, `RX dropped`, `GNSS`, `WARN` and `BOOT` lines, which `LOG` gives the phone too. Slots count
from 0 here; the app shows riders from 1.

## What comes next

- The send rule's numbers, the passengers and the slot width per kind of group, in the invitation
  (profiles, [12](12-protocol-v5-proposal.md)); the app's states, five buttons.
- Why the T-Echo's clock runs 110 ppm off; its bound is 150 meanwhile.
- The mode chosen for the group's size and freshness, carried in the invitation (T-phy).
- A shorter packet: an implicit header, the CRC left to CCM (T-length).
