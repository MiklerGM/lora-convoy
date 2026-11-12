# 12. Proposal: protocol v5, its interaction and its test rig

**2026-11-11. A proposal: nothing here is built or decided.** The owner asked for a review of v3
([PROTOCOL](PROTOCOL.md)) and of v4's plan ([09](09-protocol-next.md)), and a new proposal for the
protocol, for how the parts work together, and for the software test rig. It is numbered v5 so
that "v4" keeps meaning 09's plan, which was never given bytes. Figures are [calc] from
`tools/airtime.py` at CR 4/5, an 8-symbol preamble, explicit header and CRC; none is a measurement.

## Where we stand

- **Nothing is in use.** No board has run this firmware on a ride, and the repository records no
  run on a board at all: E0 (time) and E2 (range) are unmeasured ([07](07-experiments.md)). Every
  timing number in v3 is an assumption. So any change is free of compatibility, and no number
  deserves defending yet.
- **The idea, as the owner states it** [owner, 2026-11-11]: a group, on foot or on motorcycles,
  sees its members on a map; how often a board sends depends on the kind of group and on speed;
  a member can set a status; routes are shared, through a cloud or by packets.
- **What exists:** v3 in C (`make -C firmware test` passes), an app, two simulators, an emulator
  bench, and about 4 500 lines of documents.

## Review of v3

What holds, and stays in v5: a slot of one's own by GPS time; each board sending only its own
position; a board that works without its phone; the position's age apart from the radio contact's;
a board unsure of its time passing its slot over; the group tag; the phase drawn per frame.

What does not, most consequential first. 09 found items 4 to 8; items 1 to 3 are new here.

1. **One cadence for everything.** The interval is one number for the group, the app always sets
   2 s and SF7/250, and a stopped rider sends as often as a moving one [code: `node_poll_tx`].
   Nothing in it knows a hiker from a motorcyclist: at 5 km/h a walker moves 3 m in 2 s, under the
   GNSS's own error, and at 100 km/h a rider moves 56 m.
2. **The slot's width is a by-product, and the unmeasured term decides it.** A slot is
   interval / (n + 1), so what a board may be wrong about its time shrinks as the group grows, and
   an L1 falls silent at the eighth rider. The packet is the small term:

   | A slot, SF7/250 | Packet | Time allowance, both sides | Slot | Frame, 10 riders |
   |---|---:|---:|---:|---:|
   | v3's 25 bytes, time within ±5 ms | 30.8 ms | 10 ms | 44 ms | 0.48 s |
   | 33 bytes (v5), ±5 ms | 36.0 ms | 10 ms | 49 ms | 0.54 s |
   | 33 bytes, ±20 ms | 36.0 ms | 40 ms | 79 ms | 0.87 s |
   | 33 bytes, ±100 ms (the L1's assumed bound) | 36.0 ms | 200 ms | 239 ms | 2.63 s |

   Eight more bytes cost 5 ms a slot; an unmeasured NMEA lag costs 200 ms.
3. **Bytes were optimised, the time path was not measured.** Much care went into 24 against 25
   bytes and airtime steps; the term forty times larger waits for E0, which needs two boards and a
   day, and no protocol change.
4. **Nothing but positions.** No state before the first fix, one state (help), no way to tell the
   group anything else: a meeting point, a route.
5. **Help answered by machines.** Call numbers, answer lists and their bookkeeping; the owner has
   dropped them (D17).
6. **Protection too thin for its claims.** A 4-byte tag; a 32-bit session with no time in the
   nonce, so any two sessions of a slot that draw alike repeat nonces; a 16-bit fix time that wraps
   in 18 h; a receiver just restarted takes a recorded packet.
7. **A setup taken in pieces.** `GROUP` leaves the radio mode alone, and accepts 32 riders every
   500 ms, after which every board is silent [code].
8. **The version field is full**, and "stopped" by speed takes a walker for stopped.

## Review of v4's plan (09)

Right, and kept: the rider's state for every activity (D17), a setup computed whole (D18), a lost
fix that is not lost time (D19), drawn apart from measured (D20), the packet's freshness apart from
the fix's age, an 8-byte tag, no call numbers.

Where it falls short:

1. **It repairs v3; it does not design for the use.** D17 says "every activity", yet the plan
   keeps one cadence and one mode, and sending less when stopped stays a ticket for "after"
   (T-cadence). The two things the owner now names first, the kind of group and the speed, have no
   place in it.
2. **It pays 39 bytes for what 33 can carry.** A 64-bit session, a 32-bit time of sending and a
   32-bit fix time are three fields doing overlapping work. If the time of sending is part of the
   nonce, a 32-bit session is enough (below), and the fix's time is a 2-byte age beside it.
3. **Messages are postponed behind a heavy contract**: source, id, lifetime, duplicate
   suppression, idempotence (T-control). A record that is said again and where the latest wins
   needs one revision number, as the rider's state already shows.
4. **"State without a position" as a message type** is a flag in the one packet.
5. **A reserved byte with no purpose.** This proposal gives it one: the plan's revision.
6. **The order of work favours the model over the measurement.** Simulator metrics first, then a
   basic E0. The simulators now come to some 6 000 lines with tests and a 509-line design
   ([11](11-simulation-model.md)) whose radio parameters are, by its own word, synthetic. The two
   numbers every profile hangs on, each board's time error and the range, come only from boards.
7. **Profiles wait for measurements, rightly, but their shape need not.** What a profile contains
   can be fixed now; only its numbers wait.

## The proposal

Three ideas, and the rest follows from them:

- **A group has a profile**, chosen by its activity: radio mode, slot width, how far a rider moves
  before sending again. One group, one profile, in the invitation, taken whole.
- **A rider sends when there is something new**: after moving a set distance, on a turn, on a
  change of state, and otherwise a heartbeat. Speed sets the cadence by itself.
- **The air carries state, never messages**: each rider's own latest status, and the group's
  latest plan (a meeting point, the route in use). Both are said again, the latest wins, and no
  packet needs an answer.

### 1. The profile

The organiser picks the activity and the number of riders; the app computes the rest and refuses
what does not fit (D18). A profile holds: the radio mode; the slot's width; the slots in a frame
(riders plus one spare); the send rule's distance and heartbeat; the time allowance the slot was
sized for. The slot's width is an input:

    slot = packet's airtime + 2 × time allowance + 3 ms          frame = (riders + 1) × slot

Candidates, 33 bytes [calc]; the allowances are guesses until E0, the modes until E2. E0 has since
run on two boards: the rows it changes are redone [below](#what-the-first-measurements-changed-2026-11-11).

| Activity | Mode | Packet | Time allowance | Slot | Frame for 5 / 10 / 20 | Moved in a frame of 10 |
|---|---|---:|---:|---:|---|---|
| Motorcycles, boards with PPS | SF7/250 | 36.0 ms | ±20 ms | 79 ms | 0.47 / 0.87 / 1.66 s | 15 m at 60 km/h |
| Motorcycles, any board | SF7/250 | 36.0 ms | ±100 ms | 239 ms | 1.43 / 2.63 / 5.02 s | 44 m at 60 km/h |
| Bicycles | SF9/250 | 123.4 ms | ±100 ms | 326 ms | 1.96 / 3.59 / 6.85 s | 20 m at 20 km/h |
| On foot | SF10/125 | 452.6 ms | ±500 ms | 1 456 ms | 8.7 / 16.0 / 30.6 s | 22 m at 5 km/h |

- **A frame under 2 s buys nothing a rider sees**: the GNSS fixes once a second and OsmAnd redraws
  its layer at each update (T-osmand-opengl). Where the sum comes out shorter, the frame is 2 s and
  the spare time widens the allowance: ten PPS boards at 2 s tolerate ±71 ms each.
- **The slower the group, the more range and the less it asks of its clocks.** On foot the mode
  has some 10 dB more link budget than SF7/250 ([06](06-own-protocol.md)), and ±500 ms holds for 167
  minutes without GNSS at 50 ppm, against 7 minutes for ±20 ms. A walker's GNSS may then sleep
  between fixes, likely the larger saving on a battery (to measure, E7), and a time from the phone
  becomes good enough (section 6).
- **Slots still pay on foot.** Ten walkers sending 453 ms packets at random every 30 s would lose
  24 % to each other [calc, pure ALOHA].
- **A board that cannot keep the profile's allowance says so when it joins**, and the app tells
  the rider it will stay silent; the organiser picks "any board" or "boards with PPS".
- **Duty cycle**: sending every frame, a rider is on air 1.4 % to 3.4 % of the time in a group of
  ten, and 6.3 % at most in a group of five (bicycles): the 10 % limit binds nowhere here.

### 2. When a board sends

Its slot comes once a frame. It sends in it if any of these holds, and otherwise listens:

| Reason | Rule (numbers from the profile; first guesses) |
|---|---|
| Moved | 15 m or more from the last position it sent |
| Turned | course 30° or more from the last sent, after moving 5 m |
| State or facts changed | in each of its next three slots, so that one loss does not hide it |
| Heartbeat | 8 frames since it last sent: 21 s for motorcycles, 2 min on foot |
| The plan | someone near lacks it (section 4) |

- **Speed sets the cadence, without modes**: at 60 km/h a rider sends every frame; a walker in a
  motorcycle group every fifth; a group at a café once in eight. It is APRS's SmartBeaconing
  fitted to slots, and one rule by distance for every activity, as D17 asks of "stopped".
- **What it buys, plainly:** less air taken from neighbours (ten stopped riders use an eighth),
  duty-cycle room in the slow modes, less transmitting. It does not make positions fresher inside
  the group: the slot was the rider's own anyway.
- **What it costs:** a lost packet from a rider standing still is made up a heartbeat later, not a
  frame later. Each packet names its heartbeat, so a receiver knows by when silence means no
  contact, and the app does not call a quiet stopped rider lost.
- **A slot it does not use, it listens in**: two radios given one slot hear each other sooner than
  v3's one frame in 32, which stays for riders always on the move.

### 3. One packet: the status, 33 bytes

33 bytes cost what 31 do in every candidate mode, and v4's 39 would cost 5 ms more at SF7/250 and
41 ms more at SF10/125 [calc].

| Bytes | Field | Notes |
|---|---|---|
| 0 | version, type | bits 7–4 version (5), bits 3–0 type: 0 status, 1 plan |
| 1 | group tag | as v3 |
| 2 | slot | 0–31 |
| 3–6 | session | 32 random bits, drawn at each start |
| 7–10 | sent | UTC seconds when sent |
| 11 | count | packets this session, modulo 256 |
| 12–13 | fix age | seconds at sending; `FFFF` for older than 18 h |
| 14–19 | latitude, longitude | as v3, 24 bits each |
| 20, 21 | speed, course | as v3 |
| 22 | facts and state | bits 0–3 facts: has a position, position is current, stopped, battery low; bits 7–4 the chosen state: normal, need help, please stop, need fuel, helping, eleven free |
| 23 | plan revision | 0 for none (section 4) |
| 24 | heartbeat | bits 2–0: sends at least every 2^h frames; bits 7–3 zero |
| 25–32 | tag | CCM, 8 bytes |

Bytes 0–11 go in clear and are authenticated; 12–24 are encrypted.

- **A state with no position** is this packet with "has a position" clear: a board with a usable
  time and no first fix can say "need help" (D19).
- **Freshness and the fix's age are two fields.** A receiver with a time takes a packet only if
  `sent` is within a few seconds of its own clock, so a recording played later is refused, by a
  receiver just restarted too; one with no time yet shows ages as unknown, as in v3. The fix's
  time is `sent` less the age, and never wraps.
- **The nonce** is bytes 2–11 and three zero bytes: slot, session, sent, count. A board never
  sends twice with one (sent, count), and draws a new session if its clock steps back. A repeat
  then needs two boards in one slot with the same 32-bit session in the same second: one in 2^32
  for a slot given out twice, where v3 repeated whenever any two sessions of a slot drew alike over
  the key's life. Time in the nonce is what 802.15.4 TSCH does; the session covers what TSCH leaves
  to unique device addresses. The outside review asked for a 64-bit session for a nonce without
  time; this is the cheaper way to the same end, and it goes to the outside review of the
  cryptography with the rest (D13).
- **Per slot a receiver keeps the latest (sent, count)** and takes only later ones. v3's four
  remembered sessions go.
- **"Stopped"** by distance, as 09 proposes: two minutes within 30 m, a hypothesis until recorded
  walks and rides check it.

### 4. The plan: what the group shares

One record for the whole group, the latest winning: a meeting point and the route in use. It is
the second packet type, the same 33 bytes, sent in the sender's own slot in place of a status.

| Body, 13 bytes | Field |
|---|---|
| 1 | revision, 1–255 and round again, compared within half the range |
| 1 | author's slot; flags: has a point, has a route, route reversed |
| 6 | the point's latitude and longitude |
| 1 | the point's kind: meet here, fuel, food, camp, danger |
| 4 | the route: first four bytes of the route file's SHA-256 |

- **Any rider's app may change it**: the revision goes up by one, and of two changes made at once
  the higher author's slot wins. The author's board sends it three times, in every other slot.
- **Every status says which revision its sender holds.** A board that hears a lower one than its
  own sends the plan in its next slot, unless it heard it sent within the last frame, and at most
  once a heartbeat. A board that hears a higher one shows "the plan has changed, not received
  yet". Nobody asks and nobody answers: a status with an old revision is the request.
- **So it reaches riders the author cannot hear**, through any rider who has it, and a rider back
  from behind a ridge gets it from the first board heard.
- **The organiser's app shows who has it**: each rider's revision beside the age of their radio
  contact. That is what v3's answer lists tried to give, at no cost in air.
- **It is the reserved byte's purpose** (a change to D17's "sent as zero").

### 5. Routes

- **The route's geometry does not go over LoRa in v5.** It is a file, passed phone to phone
  before the ride by whatever the riders use, as the invitation is; the app keeps files by hash
  ([06](06-own-protocol.md), Geometry and intent).
- **Over LoRa goes the pointer**: the plan names the route in use and its direction. An app without
  that file says so, and whom to ask.
- **No cloud is built now.** A hub (T-hub) is a server to pay for and keep; it earns its place
  only if handing files over proves a nuisance on real rides. It would fit unchanged: a file
  encrypted with the group's key, found by its hash.
- **Why not packets:** 200 km of off-road track within 2 m is 7.1 kB ([06](06-own-protocol.md)).
  At 10 bytes of it a slot that is 710 slots: half an hour of one rider's positions in a
  motorcycle group, three hours on foot [calc]. A change on the way is nearly always "meet here",
  and that is the plan's point.

### 6. Time

- **One rule for every source**: a source gives a time and a bound on its error; a board sends
  while the bound, grown by drift, fits the profile's allowance. PPS and NMEA today; later the
  group's own packets, whose `sent` and slot give the frame to a board with no fix (T-timing).
- **Asked of the owner: may the phone's time open a slot** where the allowance is wide? Phone API
  6 says never. On foot, at ±500 ms, a time from the phone's own GNSS would do, and a board with
  no GNSS at all could then ride along with the phone's position ([01](01-goals.md), wishes;
  T-phone-gps). Only after the `CLOCK` exchange's real error is measured.

### 7. Phone, board, invitation

The split stays (D4): the board is the node, the phone sets up and shows. Changes, all in one
commit with both ends (D3):

- **Invitation `LC3.`** adds the profile: mode, slot width, send rule, activity.
- **`GROUP`** carries the whole profile and is taken whole or refused; `MODE`, `SLOT`, `SLOTS` and
  `INTERVAL` stay for the bench only, and are refused while a group is set.
- **`STATE <0–15>`** replaces `HELP`; **`PLAN`** sets the plan and reports it.
- **`RIDER` lines** gain `state=`, `plan=`, `beat=` and `sent=`, and lose `help=` and `answered=`.
- **The app**: the activity when making a group; five state buttons; a long press on the map for
  "meet here"; route files kept by hash; per rider, the contact's age, the position's age, the
  state, and whether they hold the plan.

### 8. Left out, on purpose

| Not in v5 | Why, and what would bring it back |
|---|---|
| Relaying positions | The plan already travels rider to rider; positions could the same way, as a third type. First a ride must show the column splits (E8) |
| Route geometry over LoRa, a hub | Section 5 |
| Text chat | An event, not a state: it needs delivery, which nothing here has. The states and the point cover the stated use |
| Sleeping between slots | Possible once slots are fixed in time; worth it only if E7 shows the receiver, not the GNSS, drains the battery |
| Two speeds in one frame, groups apart by frequency | Unchanged tickets |

## The test rig

Today there are five: host C tests; the stepped C simulator (`firmware/test/sim.c`, 1 566 lines);
its comparison runner (`tools/compare.py`); the DES simulator (`tools/simulator`, some 3 600 lines
with tests); the emulator bench. No rig holds a real board. The two simulators count a position's
age differently (on one trace, 4.5 s sampled against 4.72 s integrated at the 95th percentile,
[11](11-simulation-model.md)), and the bench takes eight minutes and 5 GB a ride.

**Proposed: one log, one evaluator, three sources.**

```
                 a scenario: who moves where, who changes state, who hears whom
        ┌────────────────────────────┬────────────────────────────┬───────────────────┐
   A. model                     B. desk                       C. ride
   DES + the C core             real boards on USB            real boards, phones
        └────────────────────────────┴────────────────────────────┴───────────────────┘
              phone-API lines with a timestamp, one JSONL file per board
                                        │
           one evaluator: the same six numbers, whatever the source
```

The phone API is already the same text on TCP, USB and Bluetooth, so the line a simulated board
prints and the line a real board prints are one format. That is the whole trick.

- **A. The model** (exists): the DES around the real C core. It answers questions of logic: do
  slots hold under a given clock error, does the send rule behave, does the plan reach everyone,
  what do two groups do to each other. Its scope is frozen: no terrain, no new reception models,
  until a desk measurement disagrees with what it has.
- **B. The desk** (missing, small): one script, `tools/rig/rig.py`. It opens each board's USB
  console, sets a group, plays a scenario's commands (`STATE`, `PLAN`, a restart) at their times,
  and writes every line with the host's clock. The firmware already prints what E0 needs
  (`lag=`, `txlead=`, `offset=`). Boards by a window for GNSS, the lowest power, the RTL-SDR as a
  witness of when packets really start.
- **C. The ride**: the app saves the lines it receives to a file; the same evaluator reads it.
- **The evaluator's six numbers**, each as a median and the worst seen:

  | Number | From |
  |---|---|
  | Age of a position on the map, outages counted | `RIDER` lines |
  | Longest gap between updates | `RIDER` lines |
  | Delay of a state, from `STATE` on one board to its `RIDER` line on another | both logs |
  | Time for a plan to reach every board in contact | `plan=` |
  | Air per board, the busiest named | `LEDGER` |
  | How late packets begin, and slots passed over | `offset=`, `txlead=`, `unsure=`, `late=` |

- **Scenarios, few and fixed**: a tight group; a column that stretches out of reach and returns;
  everyone stopped; walking pace in each profile; two groups on one channel; some boards without
  GNSS for a while; a board restarted; the plan changed while two riders are out of reach; a board
  told a wrong time.
- **The emulator bench stays** for the app and OsmAnd, run when they change; it decides nothing
  about the protocol.
- **To retire:** `tools/compare.py`, `tools/comparison` and the stepped simulator's measuring;
  its `serve` stays as the bench's boards. One engine, one set of numbers.

## Order of work

1. **The owner's answers** to the questions below.
2. **E0 on the desk with today's firmware**, both boards, through the first cut of `rig.py`. It
   turns the allowance column of section 1 from guesses into numbers, and needs no v5.
3. **v5 in the C core and the model**: codec, send rule, plan, a profile taken whole; each with a
   test that fails first. The scenarios above, on the candidate profiles.
4. **The app**: profiles, states, the plan, `LC3.`.
5. **The same scenarios on the desk boards**, then a walk and a ride with two or three: E2 per
   profile, the logs through the same evaluator.
6. **An outside review of the cryptography** before any ride with strangers (D13).

## Asked of the owner

Answered 2026-11-11 [owner]: profiles by the kind of group and by movement, yes: there is no
sense in repeating a state at a standstill, or from a walker's beacon, and the battery matters
more than the slightly better chance of being heard. The bytes left empty have no purpose planned,
so the reserved byte is free. The tests come first ([13](13-test-plan.md)). The rest is open.

1. Profiles by activity, with the radio mode in the invitation? **Yes.**
2. Sending by movement, with a heartbeat, in place of every frame? **Yes.**
3. One 33-byte packet as laid out, "no position" a flag in it?
4. The plan, with its revision in the byte D17 reserved as zero? The byte is free; the plan
   itself is not yet answered.
5. Routes as files off the air and a pointer on it; no hub and no geometry over LoRa for now?
6. The phone's time opening a slot in wide-slot profiles, once measured?
7. The rig: a desk script and one evaluator before any more simulator; `compare.py` retired?

## What would change it

- **E0 worse than ±100 ms on the L1**, or a lag that wanders: the motorcycle profile needs PPS
  boards, or a longer frame.
- **E2 showing the fast modes too short for a column**: slower modes for motorcycles, and then the
  duty cycle, not the frame, sets the pace.
- **A ride showing the column splits behind terrain** as a matter of course: relaying moves from
  "left out" to next.
- **Walkers' batteries drained by the receiver**: sleeping between slots.

## What the first measurements changed (2026-11-11)

Stages 0–3 of [13](13-test-plan.md) ran indoors on a Heltec V4.3 (PPS) and an L1 (none), with an SDR
as witness ([07](07-experiments.md), results). Against this proposal:

- **A board without PPS needs ±50 ms, not ±100** [bench], once its L76K sends GGA and RMC only and
  the board is told its lag. The rows that changes [calc]; the PPS row stays a guess, as nothing
  but the Heltec itself has judged the Heltec's time:

  | Activity | Mode | Packet | Time allowance | Slot | Frame for 5 / 10 / 20 | Moved in a frame of 10 |
  |---|---|---:|---:|---:|---|---|
  | Motorcycles, any board | SF7/250 | 36.0 ms | ±50 ms | 139 ms | 0.83 / 1.53 / 2.92 s | 25 m at 60 km/h |
  | Bicycles | SF9/250 | 123.4 ms | ±50 ms | 226 ms | 1.36 / 2.49 / 4.75 s | 14 m at 20 km/h |

- **The allowance is still the slot**: 100 of 139 ms. The packet's bytes remain the smaller term,
  and the next gain is in the clock, not in the format.
- **A profile sized to a board's own bound leaves it no holdover.** The L1 claims ±50 ms; in a
  ±50 ms profile it falls silent the moment its GNSS does. Its clock ran 29 ppm fast for an hour, a
  straight line (residual 0.9 ms), while its bound grew at the 50 ppm it assumes. Either the
  allowance carries a reserve (15 ms buys 5 minutes at 50 ppm: a 169 ms slot, ten riders in 1.86 s),
  or the board learns its drift while it has GNSS, and an hour without then costs a few ms.
- **The clock should be a line through many marks, not the last mark.** One NMEA mark is off by
  8.5 ms (sd), with a late tail: 2 packets in some 4,400 beyond ±50 ms, to 62. The crystal between
  marks is far steadier than the marks. A fit over a minute of them would bring the noise under
  2 ms, cut the tail and give the drift above in the same stroke. What it cannot remove is the
  centre moving with the sky: −12 to +8 ms over the day, as the satellites in view went from 5 to
  14. How far that goes outdoors decides whether a board without PPS can share the PPS row.
- **A wrong clock goes unseen** (13, 3.3): a board 181 ms off sat on its neighbour's slot in every
  frame and no counter moved. Every receiver can tell how far a packet began from its slot; a board
  that finds its whole group shifted by the same amount is the one that is off, and should stop
  sending. That check is also the first half of time from the group's packets (section 6).
- **The fixed costs are known and small**: the radio's start, 7–8 ms and steady to 0.2 ms, now sent
  ahead; a packet 0.2 ms longer on air than computed; a mode change taken before the next slot. The
  3 ms in the slot's formula stands.
- **What held**: slots by GNSS time on two unlike boards for hours with no overlap; a shared slot
  heard as a clash within 21 s; two groups on one channel losing 4 % to each other (3 % computed);
  the duty ledger within 1.2 % of the air.

Still unknown, in the order it would change the design: the lag and the drift outdoors (sky, cold,
movement); range per mode (E2), which picks the modes; a second L1, to tell one board's numbers
from the model's; a PPS board judged from outside (the T-Echo); what a board draws listening,
fixing and asleep (E7), which decides whether the walking profile is about sending less or about
sleeping between slots; and real tracks, to set the send rule's distance and heartbeat by.
