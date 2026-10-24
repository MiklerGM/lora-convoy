# 09. The protocol's next version

**2026-10-24.** The owner [owner]: comparative measures of v3 in the simulator first; no profile
fixed yet; help without a position goes into v4's agreed change, not as a fix to v3; a basic E0
early, before the fixes and v4. A re-check the same day [recheck] added two decisions, the mode in
use being the mode applied (D15) and times keeping their meaning to the phone (D16), refined E0's
method, and bounded what the simulator's numbers say. Later the same day the owner set the
participant's state for every activity, with no automatic answers to a call for help (below). An
outside review of v3 [review] went over the format, the
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
   the longest gap without any update; the time until the others' maps show a call for help. Counted
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
3. **What a call for help promises** [owner]: a state the caller's packets carry until it is called
   off, and no more. The interface shows that it is going out and how fresh the radio contact with
   each rider is, and promises no delivery of a given call (the participant's state, below). It
   goes out before the first fix too, in v4's change. Still open: a call from the
   board's button with the phone gone, and whether it outlives a restart or a loss of power.

## The participant's state, for every activity

2026-10-24 [owner]. The boards are for motorcyclists, cyclists, hikers and mixed groups. The format
carries a rider's position, its freshness and the rider's state, alike for all; an activity shows
only in settings and in which buttons the interface offers. A code means the same for everyone:
"need fuel" is fuel.

- **Facts the board observes**, any of them at once: stopped; a current position, the GNSS having
  a fix; its battery low.
- **One state the rider chooses**: normal; need help; please stop, asking the group to; need fuel;
  helping. Stopped is a fact, please stop a request to the group, and helping a person's choice,
  which no radio's receipt of a packet can stand for.
- **Said again in every packet** until changed or called off, as the facts are. A state repeated so
  needs no number of its own for each change, unless a concrete need for one is found.
- **No automatic answers to a call for help.** v4 has no field for "whose call my
  radio heard", no call number kept only to match answers to calls, no lists of the boards that
  answered, and no word to the phone when those change. That a board hears a neighbour does not
  show the neighbour hears it, and that is accepted: the interface shows that a state is going out
  and how fresh the radio contact with each rider is, and promises no delivery of a given call. Who
  hears whom is a question for the link's diagnosis in general, later, with no new load on the air
  for it now.
- **The byte freed by the answers** stays, reserved and sent as zero, until a real need is found
  for it; the packet is not shrunk to 24 bytes for its sake. The authentication tag keeps its whole
  length; and v4's protection and time are not bound to 25 bytes: their bytes are
  counted when the whole layout is agreed.
- **When a rider counts as stopped.** v3's rule, a minute under 5 km/h and moving again over
  10 km/h, takes a walker for stopped within a minute and never moving again, and a cyclist
  climbing at 6–8 km/h after a stop for stopped still [code]. A rule for every activity, by distance
  rather than speed: stopped after two minutes within 30 m of one point, moving once out of it. A
  hypothesis, to be checked against recorded rides, walks and climbs before it is kept.
- **When a position turns old** is not tied to the rider's last speed: a speed of 0 does not show
  that the rider is still standing once contact is lost.
- **Open**: helping a rider named; and whether the chosen state outlives a restart of the board.

For instance: a motorcyclist refuelling, stopped and in the normal state; a cyclist with a puncture,
stopped and asking the group to stop; a hiker fallen into a gully, needing help, stopped, with no
current position, the last one shown with its age; the rider who turns back for the hiker, helping.

**v3 has, of this**: the facts of a current position and of a stop, the stop by its speed rule, and
a bit for a low battery that nothing sets yet; one chosen state, need help, with every board that
hears it answering automatically, the call's number to match the answers, and their lists on the
phones. It stays as it is until v4 is built.

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
- **The state without a position** [owner: in v4's change]. Before its first fix a
  board sends nothing, its rider's state included, though its time may be good enough for its slot
  already (`node_poll_tx`) [code]. With a usable time it sends the state, marked as without a
  position.
- **The participant's state** (above): the observed facts, and one chosen state of five, room left
  for more.
- **No call number** [owner]. v3's seven numbers repeat, a new call taking an old one's number once
  in seven after a restart [review]; with no automatic answers to match, the number goes, and a
  state repeated needs none.
- **A reserved byte**, sent as zero, where v3 answers calls.
- **Message types**, then: a position with the rider's state, the state without a position, later
  control and routes, each with its source, id, lifetime, duplicates dropped, idempotent, a call
  for help first (T-control) [review].
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
  each link want measuring: that a board hears a rider does not show the rider hears it. A higher SF hears further, sends
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
- Two groups side by side, with losses in bursts; several riders in a state at once; a board
  restarted while in one.
- Measured as the map's age, the gaps' lengths and the time until the others' maps show a state,
  not only the share delivered. The simulator can count all of them before any hardware.
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
  median and the worst. This and the next measure v3's automatic answers, which v4 drops
  [owner]; they stay for the record. How soon the others hear a call, the measure that stays, is
  not counted yet:

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
   how soon a call for help reaches the others with several callers (on v3 counted so far only
   through its answers), two groups with burst losses. v4's choices
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
   the participant's state, the state without a position, the reserved byte and the message
   types; then the bytes, then the
   profiles recomputed. One change of firmware, app and docs together (D3), and an outside review
   of it.
6. **The boards again**: E2 with real mounting, and the full E0; only then are profiles fixed
   [owner], and this document is revised.
