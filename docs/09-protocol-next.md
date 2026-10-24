# 09. The protocol's next version

**2026-10-24.** The owner [owner]: comparative measures of v3 in the simulator first; no profile
fixed yet; help without a position goes into v4's agreed change, not as a fix to v3; a basic E0
early, before the fixes and v4. A re-check the same day [recheck] added two decisions, the mode in
use being the mode applied (D15) and times keeping their meaning to the phone (D16), refined E0's
method, and bounded what the simulator's numbers say. An outside review of v3 [review] went over the format, the
schedule, help and the air. Its findings are checked here against the code [code] and recomputed
with `tools/airtime.py` at v3's preamble of 8 [calc]; each figure is a bound from the model, not a
measurement. What follows is what to keep, what the owner decides first, and in what order the
rest gets settled. It continues [06](06-own-protocol.md)'s open questions; decisions go to
[ARCHITECTURE](ARCHITECTURE.md) and the tickets of [BACKLOG](BACKLOG.md) as they are made.

## What stays

Personal slots, each rider sending their own position, a board that runs without its phone, and
two ages, the position's and the radio's. One fixed roster a ride, with an invitation for each
rider. The rule that a board unsure of its time passes its slot over rather than risk a
neighbour's. The phase drawn per frame, so two groups on one channel do not collide every frame.
One key for a trusted group, any member of which can send as any slot ([SECURITY](../SECURITY.md)).

## First: what the owner decides

1. **The scenario, and its three numbers.** Finding a rider who fell behind, seeing who turned off
   at a fork, and following the bike ahead need different freshness. At 60 km/h a bike covers 33 m
   in 2 s, 83 m in 5 s and 500 m in 30 s [calc], so 30 s of freshness and 60 m of error are not
   one requirement said twice. Wanted, for the main scenario: the age of the position on the map;
   the longest gap without any update; the time to the first answer to a call for help. Counted
   with the outages in: a rider behind a ridge for 5 % of a ride bounds the 95th percentile,
   however often everyone sends [review].
2. **The group's size against its freshness, and the L1 without PPS.** With NMEA's ±100 ms, as the
   L1 has it, v3's schedule needs at least [calc]:

   | Mode, v3's 25 bytes | On air | Frame for 10 riders | for 20 |
   |---|---:|---:|---:|
   | SF7/250 | 30.848 ms | 2.58 s | 4.93 s |
   | SF9/250 | 102.912 ms | 3.38 s | 6.45 s |
   | SF10/125 | 411.648 ms | 6.77 s | 12.93 s |

   At today's 2 s an L1 is silent from the eighth rider on [code]. So: 1–2 s means PPS on every
   board, or NMEA measured much better than ±100 ms; otherwise about 5 s for ten riders and 10 s
   for twenty, which changes how the map feels. The review would take ten riders as the main
   profile and twenty with slower updates [review].
3. **What a call for help promises.** Whether it works without the phone, from the board's button;
   whether it outlives a loss of power; whether it goes out before the first fix, or before the
   board has any time at all. And what "answered" means: that a board received the call, by
   radio, not that a person saw it or is coming [review].

## Fixes that need no new layout

- **A group's radio profile, checked whole.** `GROUP` leaves the mode alone, which `MODE` keeps
  apart, so a board can join a group and stay on a mode the others do not use. `GROUP` takes 32
  riders every 500 ms, a slot spacing of 15.15 ms under a 30.848 ms packet, and the board then
  stays silent [code]. A setup should be refused unless its slots fit its frame with the board's
  own time error; the mode should come with the group (T-phy), or one PHY be fixed for now.
- **The region bounding the settings.** `MODE … 500` is taken at 869.525 MHz, and 500 kHz spills
  out of 869.40–869.65 MHz, the window whose 10 % the budget is for [code] ([08](08-regions.md)).
  Settings the region does not allow are refused; some duty cycle is kept for help and control;
  a board's restart does not start a new hour of duty [review].
- **The mode applied before it is used** (D15). `MODE` changes the core's mode at once, and the
  radio task sends one more packet on the old mode before it applies the new: the schedule and the
  ledger reckon with one mode while the air carries another [code] [recheck].
- **Times kept to the phone** (D16). RIDER lines carry ages reckoned when they were made, and the
  app counts them from the line's arrival, so a line's wait in the queue and on the air is lost: a
  position 29 s old, delivered 5 s later, shows 29 s old and fresh [code] [recheck]. And a caller
  hears of an answer only with the next status, up to 10 s after its board did: a change in who
  answers goes out at once.
- **The bounds of time.** An NMEA fix parsed more than a second after its edge is paired with the
  next edge, a second out, and `lag=` shows a plausible 100 ms [code]. The guard counts the error
  and `TX_ERR_US`, 1 ms, which the known path already exceeds: on the Heltec the front end's switch
  alone waits 1 ms before SetTx (`fem_transmit`), and the packet's making and SPI come on top; and a
  send may start up to 2 ms late besides [code] [recheck]. One bound, from the scheduled moment to
  the radio's first RF, measured (E0), and a check that the marks agree with each other.

## The next layout: decided together, then the bytes

- **The version field is full.** Two bits, and v3 holds the last value: v4 needs another header,
  not `PKT_VERSION + 1` [code].
- **An 8-byte tag.** Below 64 bits, NIST SP 800-38C asks for an analysis of the risk and of how
  many failed checks a key may see; a forged packet need not be random bytes [review].
- **A nonce from a 64-bit session**, with 64 bits of real entropy, its counter and its slot:
  unique however the clocks go. Not the frame number alone, which a duplicated slot, a time set
  back, a changed interval or a second message in a frame would repeat; and under one key a
  repeated nonce undoes CCM ([RFC 3610](https://www.rfc-editor.org/rfc/rfc3610#section-2.1))
  [review].
- **The packet's freshness apart from the fix's age.** An authenticated time of sending, or frame
  number, taken within a window: a receiver just restarted no longer takes an old recorded packet,
  and a board can say it is alive while its position is old. The fix's age beside it, saturating
  at a value that means "older than that": v3's fix time of 16 bits is unwrapped within 9 h 6 min,
  and past 18 h 12 min an old fix looks recent [code] [review].
- **Help without a position** [owner: in v4's change]. Before its first fix a board sends nothing,
  its call for help and its answers to others' included, though its time may be good enough for
  its slot already (`node_poll_tx`) [code]. With a usable time it sends its status, its call and
  its answers, marked as without a position.
- **The identity of a call for help.** Seven numbers repeat: after a restart a new call takes the
  old one's number once in seven, and seven calls go round. A generation kept over restarts, or a
  random token big enough [review]. Or a short message of its own, help and answers, sent in a
  slot instead of a position, so positions do not grow for it.
- **Message types**, then: position, help and answers, status without a position, later control
  and routes, each with its source, id, lifetime, duplicates dropped, idempotent, help first
  (T-control) [review].
- **The header.** An implicit LoRa header saves 2.56 ms at 25 bytes on SF7/250 (30.848 to
  28.288 ms); dropping the PHY's CRC saves nothing there [calc].
- **The cost**, for scale and not yet a layout, at SF7/250 with v3's explicit header and CRC [calc]:
  an 8-byte tag, 29 bytes, 33.408 ms; and a 64-bit session, 33 bytes, 35.968 ms; and a time of
  sending and a fix time of 32 bits each, 39 bytes, 41.088 ms.
- **The cryptography's own code** (D13): an outside audit, or a maintained library [review].

## The air and the range

- **A shared air.** Our sync word filters others' packets out; it does not keep their energy out.
  One SX1262 hears one frequency at one data rate at a time. The phase between two groups loses
  packets in bursts, which an average delivered share hides: report the longest gap of the whole
  group, and the delay of a call for help [review]. At 2 s on SF7/250, ten riders take 15.4 % of
  the air and twenty 30.8 % [calc], before anyone else's traffic.
- **Listening before sending** never waits into another slot: a board that hears the channel busy
  passes its slot over and sends its state, current by then, in the next. A stopped board may pass
  some of its slots the same way [review].
- **Range** is antennas, mounting and terrain before it is a link budget. Over 4 km at 869.5 MHz
  the first Fresnel zone is 18.6 m in radius at mid-path [calc], far above a bike's antenna.
  Mounting, polarisation, the rider's body, metal, the amplifier's supply and both directions of
  each link want measuring, the answers' way back included. A higher SF hears further, sends
  longer, and is longer exposed to interference. If a column splits behind terrain, one plain
  relay helps where sending faster does not (T-relay) [review].
- **The roster** stays fixed a ride: a late rider takes a slot kept for them; a change of frame,
  roster or PHY is a new setup at a regroup, and on the move would need an epoch to switch at.
  Two radios in one slot are found in some 33 s by the first of the two and 66 s by a given one, at
  2 s frames [calc]: a diagnosis, not a repair (T-join) [review].

## What would change the decisions

- **E0** over the whole time path, to the actual start of RF: UART, parsing, locks, the packet's
  making, SPI ([07](07-experiments.md)).
- **E2** with the antennas mounted as on a ride.
- Two groups side by side, with losses in bursts; several calls for help at once; a board
  restarted during a call.
- Measured as the map's age, the gaps' lengths and the time to the first answer, not only the
  share delivered. The simulator can count all of them before any hardware.
- If 5–10 s and small groups are enough, GNSS-timed slots are to be weighed again against simpler
  access and the existing baselines ([03](03-alternatives.md)); if twenty riders at 1–2 s are a
  must, their case is much stronger [review].

## v3, measured in the simulator

2026-10-24, `make -C firmware sim`: the node core as built, on simulated air [sim]. A model of v3
to compare v4's choices with, and no more [recheck]: every board here has PPS and a perfect
crystal, the air starts the moment `node_poll_tx` gives a packet, past the real driver and its
timing, and an age is the one a board shows, not the one OsmAnd shows after the phone. So these are
not numbers for today's L1, nor freshness on the map. A longest gap is the longest observed in one
run, which another seed moves: 12.0 s here became 18.0 s with only the seed changed, delivery and
the 95th percentile staying put. For comparing v4, a small fixed series of seeds, each measure
given as its median over the series and the largest observed.

- **Gaps**, the longest observed. Three riders, nothing lost: 4 s, a frame and one spent listening
  in its slot. With 10 % lost at random, 10 s. Twenty riders in slots, 6 s.
- **Two groups of ten on one channel**, the same frame: 68.2 % delivered, but the losses come in
  bursts, when the two groups' phases meet: the position a board shows is 6.5 s old at the 95th
  percentile, and in this run a rider went up to 12 s without another, and up to 11 s hearing none
  of its group.
- **The outages counted in.** The column of twenty spreading to 9.5 km, radios sure within 1 km:
  in reach, fixes 2.5 s old at the 95th percentile; over every pair, in reach or not, 4 s at the
  median and 966 s at the 95th percentile, and older than 30 s 45 % of the time.
- **Several calls for help at once**, among twenty riders with 10 % lost, ten rounds each, the
  median and the worst:

  | Callers at once | First answer | All 19 answers |
  |---:|---:|---:|
  | 1 | 0.9 s, 2.1 s | 6.3 s, 8.2 s |
  | 2 | 1.3 s, 3.4 s | 7.9 s, 11.7 s |
  | 4 | 1.6 s, 4.6 s | 13.9 s, 34.2 s |

- **A caller restarted during its call**, calling again at once: 7 of 40 new calls were answered
  before they went out at all, by answers to the old call that took the same number.

## How it is settled

1. **The owner's decisions** above: the scenario's numbers, the size against freshness and PPS,
   what help promises. No profile is fixed before them [owner].
2. **The simulator measures what matters**, on v3: the map's age with outages in, the longest gap,
   the time to the first answer with several callers, two groups with burst losses. v4's choices
   are then compared on numbers.
3. **A basic E0 on the boards** [owner: early], measured as [recheck] refines it:
   - sending: from the scheduled moment to the actual start of RF, taken as the radio's TxDone
     less the packet's airtime, the event stamped by an interrupt whose error is known, on both
     boards; the front end's 1 ms, the packet's making and SPI all fall inside;
   - receiving: RxDone on the receiver's UTC less the sender's scheduled start and the airtime,
     not against the slot's end, which hides lateness: at ten riders a packet 100 ms late still
     ends some 51 ms before its slot's boundary;
   - the losses counted beside the offsets, as the latest packets may be the ones that collided;
     the driver's poll every 5 ms shows an L1's large offset, never the 1 ms bound;
   - the Heltec's NMEA lag taken for the L1 only as a first guess: the measure includes each
     platform's UART and processing.
   Its numbers bound the schedule before anything is fixed on it. The boards measure both now:
   `txlead=` in the status and `offset=` in each RIDER line, from DIO1's edges stamped by an
   interrupt, whose error is the clock's step, a microsecond on the Heltec and 31 µs on the L1, plus
   however long the handler waits to run, which is still to be measured; the simulator
   checks the arithmetic, an L1 told nothing of its 100 ms lag heard 100.5 ms late ([BENCH](BENCH.md),
   E0).
4. **The fixes without a new layout**, one at a time, as in the review rounds before: a profile
   checked whole, the region's bounds, the bounds of time.
5. **v4**, item by item: header and version, tag, session and nonce, freshness and the fix's age,
   help's identity, help without a position and the message types; then the bytes, then the
   profiles recomputed. One change of firmware, app and docs together (D3), and an outside review
   of it.
6. **The boards again**: E2 with real mounting, and the full E0; only then are profiles fixed
   [owner], and this document is revised.
