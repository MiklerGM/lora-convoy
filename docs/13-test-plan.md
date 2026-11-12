# 13. Test plan: two boards and an SDR

**2026-11-11. A plan; nothing in it has run.** What to test with what the owner has: a Heltec
V4.3 with its GNSS kit (PPS), a Wio Tracker L1 Pro (no PPS), an RTL-SDR, a computer and a phone.
Stages 1 to 4 need only today's firmware (v3) and turn the guesses of
[12](12-protocol-v5-proposal.md) into numbers; stage 5 waits for v5. Flashing and the console
commands are in [BENCH](BENCH.md); expected values are [calc] or [code] and are written here
before any run, so that a run can disagree with them.

## What each tool is for

| Tool | Tells us | Cannot tell us |
|---|---|---|
| The Heltec, with PPS | the reference time: how late the L1's packets begin (`offset=`), its own NMEA lag (`lag=`) | its own lateness, until a second PPS board judges it (the T-Echo, below) |
| The L1 | how a board without PPS behaves, which is what limits the group | |
| The RTL-SDR | when RF really starts and ends, whoever sends: gaps between slots, overlaps, a packet's length, silence when a board should be silent, where the signal sits in the band, who else is on the channel | the packets' contents (no decoder here, and none needed: a packet the other board takes has passed its 4-byte tag), absolute power, anything sent: it only listens |
| Two boards | each other's bytes, a slot's neighbour, range one way and the other | collisions of three, a relay, a plan passed on through a third rider |

Two boards can still stand as neighbours in a large group: given slots 3 and 4 of 10, they send
182 ms apart, as ten riders would [code: `GROUP <key> <slot> <slots> <interval>`].

## Rules for every run

- **An antenna on each board before power.** The Heltec's amplifier reaches 28 dBm.
- **On the desk:** the default power, `POWER 3`, the weakest: 14 dBm, as `dbm=` in `STATUS`
  shows. `POWER 0` is the strongest, not the least [code]. Boards a metre or more apart, the SDR
  without an antenna and at low gain; a capture whose bursts clip is redone.
- **Logs:** one file per board per run, with the computer's time on every line:
  `pio device monitor -p <port> -b 115200 --filter time --filter log2file`. No script is needed
  before stage 5.
- **Captures:** `rtl_sdr -f 869525000 -s 1024000 -g <gain> -n <samples> run.cu8`, read with
  dmd2-lora-bridge's `bench/sdr_lora.py run.cu8 --fs 1024000 --sf 7 --bw 250000 --min-ms 15`
  (needs numpy). Five minutes are 614 MB: captures and firmware dumps go to
  a directory outside the repository, logs to `lab/`, neither into git.
- **Each run's note:** date, firmware commit, both boards' `STATUS` before and after, where the
  boards stood, what was expected, what came.
- **What the detector resolves:** bursts are found in 1 ms blocks, so a start is known to about a
  millisecond: enough for a question of 100 ms, not for one of 100 µs.

## Stage 0. Before anything is overwritten

1. Dump what each board runs now (the L1 has Meshtastic 2.7.26 with its settings, the Heltec a
   dmd2-lora-bridge build) to `bench-backup`, so either can go back.
2. `make -C firmware esp nrf`; flash both ([BENCH](BENCH.md)).
3. `rtl_test`; a 10 s capture of the empty channel; the script runs on it.

## Stage 1. Each board alone

| Check | Expected | If not |
|---|---|---|
| The start line names protocol v3, phone API 7, the board | as [BENCH](BENCH.md) §1 | the image or the port is wrong |
| A fix by a window or outdoors within minutes: `GNSS fix=1 time=1` | yes on both; the Heltec's kit has never run | VEXT (GPIO 36) on the Heltec; the L76K's pins |
| `STATUS`: `time=pps` on the Heltec, `time=nmea` on the L1 | yes | the PPS pin (GPIO 41) |
| No key set: a 60 s capture | no bursts at all | the board sends without a key: stop, a fault |
| `kept=1`, and the key still there after a power cycle | yes | flash storage |

## Stage 2. Time (E0): the number everything hangs on

Both boards in a group of two at 2 s: slots 667 ms apart, so a packet 100 ms late still lands
clear. `GROUP 000102030405060708090a0b0c0d0e0f 0 2 2000` on the Heltec, `… 1 2 2000` on the L1.

| # | Measure | How | Expected before the run |
|---|---|---|---|
| 2.1 | The L76K's NMEA lag behind its PPS | Heltec `STATUS` `lag=last/least/most`, every 10 s for 30 min | unknown; the code assumes 100 ms. Far under a second, or the pairing is wrong (T-timing) |
| 2.2 | From a slot's moment to RF, each board | `txlead=least/mean/most` after 30 min | over the 1 ms the code allows: the Heltec's front end alone waits 1 ms [code] |
| 2.3 | How late the L1 begins | the Heltec's `RIDER` line for the L1: `offset=least/mean/most`, with `rx=` beside it | mean near the L1's own lag, some 100 ms; the spread is the number wanted |
| 2.4 | The same, by the SDR | a 5 min capture: the start of each L1 burst less the start of the Heltec's burst in the same frame, less 667 ms. The boards are told apart by level or carrier offset | equal to 2.3's mean within 2 ms. If not, a board's stamps are wrong, and the SDR is believed |
| 2.5 | A packet's length | the bursts' lengths in the same capture | 30.8 ms each, within the detector's millisecond |
| 2.6 | The L1 with its lag told | rebuild it with `-DNODE_NMEA_LAG_US=<2.3's mean>` and `-DNODE_NMEA_ERR_US=<half 2.3's spread, plus a margin>`; repeat 2.3 and 2.4 | mean within a few ms of zero; the spread unchanged |
| 2.7 | Drift without GNSS | take one board's GNSS away while it runs (below) for an hour; the other's `offset=` for it, or the SDR's gap, every 5 min; its own `err=` and `unsure=` | `err=` grows 3 ms a minute (50 ppm). The real drift is the slope of the gap: the board's crystal, likely well under 50 ppm indoors |

**Taking the GNSS away.** The board must stay on, or it loses its time with its GNSS and there
is no drift to watch. Both boards can switch their GNSS from the firmware: the Heltec's kit sits
behind a load switch (`PIN_GNSS_EN`), and the L1's L76K has a wake pin (P1.09, `PIN_GNSS_WAKE`),
both already driven at start [code: `firmware/src/main.c`, `firmware/zephyr/src/board.h`;
Meshtastic's `PIN_GPS_STANDBY`]. A console command for it, `GNSS 0|1`, is to be added before
2.7, for tests only; whether the L76K falls quite silent in standby is itself to be seen. The
Heltec's kit also comes off by hand [owner], with the power off: that gives a board with no GNSS
from cold, which is 3.9, not this.

**What comes out:** for each board, lag, spread, `txlead=` at most, and drift. The time allowance a
board needs is its spread plus its `txlead=` plus a margin; it replaces the ±20 ms, ±100 ms and
±500 ms guessed in [12](12-protocol-v5-proposal.md), and the frame for each group size follows.

**What would stop the plan:** an L1 whose lateness wanders by more than 100 ms. Slots faster than
about 3 s for ten then need PPS boards, and the choice of boards is the next question, before v5.

## Stage 3. Neighbours: the schedule on real radios

The L1 built as in 2.6. One hour each unless said, a 5 min capture in each.

| # | Setup | Expected | The SDR checks |
|---|---|---|---|
| 3.1 | Slots 3 and 4 of 10 at 2 s (182 ms apart, 74 ms of time budget) | both send, `unsure=0`, `late=0`, `rx=` near all | the gap between the two bursts is 151 ms and never closes |
| 3.2 | The same of 20 (95 ms, budget 31 ms), then of 32 (61 ms, 14 ms) | the L1 sends while its `err=` fits, and is silent, counting `unsure=`, when not; never a collision | no overlap in an hour; which group size is the last that holds |
| 3.3 | 3.1 with the L1 told a lag 100 ms wrong, and sure of it | the Heltec stops hearing it: the model says they collide in every frame | the bursts overlap: the guard is what keeps them apart, and it rests on a true lag |
| 3.4 | Both given slot 3 | `clash=` on one within a minute or so (33 s on average for the first) | the bursts lie on top of each other |
| 3.5 | Two keys, each board alone in its group of 10 | `foreign=` grows; no rider shown | overlaps in scattered frames, some 3 %, never in every frame |
| 3.6 | `MODE 9 250` on both while running, then back | `held=` counts for a slot or two, then sending resumes (D15) | every burst is 30.8 ms or 102.9 ms, none of the old length after the change |
| 3.7 | One board restarted in the middle | it is heard again within a few frames, with a new session; `replay=` stays 0 | |
| 3.8 | An hour of 3.1, then `LEDGER` | `used_ms` some 54 s: 1 800 slots, less the one in 32 spent listening, 31 ms each | the sum of that board's bursts over the hour within 2 % |
| 3.9 | A board with a key and no fix yet, from cold: the Heltec with its kit taken off | silent | no bursts |

## Stage 4. Range (E2), outdoors

One board stays, high and in the clear, on the computer; the other is carried along one fixed
path, out and back. One walk per mode, both boards set to it first: `MODE 7 250`, `MODE 9 250`,
`MODE 10 125`, at the default 14 dBm (`POWER 3`).

- **Walker to home needs no log on the walker**: the home board's `RIDER` lines carry the
  walker's own position, so each packet taken has its distance, `rssi=` and `snr=`, and `rx=`
  counts the ones lost.
- **Home to walker**: the phone on the walker's board, the app's contact age watched, or a
  laptop in a rucksack. The two directions may differ: the boards' radios and antennas do.
- **Expected** [calc, [02](02-radio-basics.md)]: SF10/125 some 10 dB ahead of SF7/250 in link
  budget; how many metres that is on this path is the question.
- **What comes out:** the share delivered against distance, per mode and direction: the modes of
  [12](12-protocol-v5-proposal.md)'s profiles. A walk says little of a motorcycle's mounting,
  which is a later run.
- **Before the first walk**, one evening of `rtl_power` over 869.40–869.65 MHz at home, and later
  one on the trail (E10): who else is on the channel, as a condition of every run.

## Stage 5. v5, when it is built

The same two boards, after v5 passes in the C tests and the model. Here a small script,
`tools/rig/rig.py`, earns its place: it types commands at set times on both consoles and keeps
the timestamped lines, both boards on one computer's clock.

| # | Check | How | Expected |
|---|---|---|---|
| 5.1 | A profile is taken whole | `GROUP` with each profile; one that does not fit | mode, slot and rule all change together, or nothing does |
| 5.2 | A board lying still | an hour on the desk; the SDR counts | one burst in eight frames, the heartbeat; `stopped` set after two minutes |
| 5.3 | A board carried at walking pace, in each profile | a walk with the phone logging | motorcycle profile: a packet about every fifth frame; on foot: every frame |
| 5.4 | A change of state | `STATE 1` on one board at a known moment, a hundred times; the other's `RIDER` line | the delay's median and worst, in frames; three sends after each change on the SDR |
| 5.5 | A state with no position | a board with a time and no fix | a status with "has a position" clear is heard; nothing from a board with no time |
| 5.6 | The plan | set on A while B is off; B switched on | B has it within a few frames of being heard; both `plan=` equal. Through a third rider: not testable with two boards |
| 5.7 | Stage 3 again | 3.1 to 3.9 on v5's 33 bytes | a burst is 36.0 ms; the rest as before |

A recorded packet played back cannot be tried here, since the SDR does not transmit: refusing it
stays with the C tests.

## Stage 6. Battery

The owner's reason for sending less [owner, 2026-11-11]. It needs a USB power meter, or patience:
a full battery run down with the time noted.

| Run, same board, same battery | Tells |
|---|---|
| Radio listening, GNSS on, sending every frame | the base |
| The same, sending one frame in eight | what sending costs |
| The same, GNSS off (time held) | what the GNSS costs |
| Bluetooth connected against not | what the phone's link costs |

Whichever is largest is what a walker's profile should save first: the proposal guesses the GNSS,
and leaves sleeping between slots out until this says otherwise.

## Stage 7. The phone and a real board

Only real boards have their own Bluetooth: pairing, an encrypted link, a bond kept over a restart,
`FORGET` ([BENCH](BENCH.md) §4). Then a soak, three hours with the phone asleep in a pocket and
OsmAnd navigating (E7): stretches with no update shown are counted.

## Order, and what each stage decides

| Stage | Roughly | Decides |
|---|---|---|
| 0, 1 | an evening | the boards and the SDR work |
| 2 | a day | the time allowance per board: whether an L1 can ride in a fast group at all |
| 3 | two days, mostly waiting | whether the schedule holds on radios as in the model; the largest group at 2 s |
| 4 | three walks | the radio mode of each profile |
| 5 | after v5 | v5 behaves on boards as in the model |
| 6, 7 | alongside | what to save on a battery; whether the app lasts a ride |

Results go to [07](07-experiments.md) with their conditions, and the numbers into
[12](12-protocol-v5-proposal.md)'s profile table.

## When more boards come

A second L1 Pro and a LILYGO T-Echo are on their way [owner, 2026-11-11]. The T-Echo is an
nRF52840 with an SX1262 and an L76K whose PPS is wired, to P1.04 [code: Meshtastic's
`variants/nrf52840/t-echo`; which GNSS a given unit carries is seen at its first start]: the
L1's processor and GNSS, with the line the L1 lacks. Stages 0 to 2 wait for
neither.

| With | New test | Why it matters |
|---|---|---|
| The second L1 | 2.3 and 2.4 for both L1s, the Heltec judging | whether the lag and its spread belong to the model or to the unit: one build setting for every L1, or one per board |
| | 3.1 and 3.2 with the two L1s as neighbours, the Heltec and the SDR watching | the worst pair a group of "any boards" has: two boards without PPS side by side. This sets that profile's slot |
| | 3.4 with a bystander | a clash seen by a third board, as a group would see it |
| Three boards of ours | two sending into one receiver at once (3.3, 3.4) | what the receiver makes of an overlap: one packet, or none |
| | a chain: A hears B, B hears C, A does not hear C, by distance or by a wall | 5.6's plan passed on through a rider; later, relaying. First show that A does not hear C |
| The T-Echo, as it comes | stock Meshtastic on ShortFast, which is our SF7/250 on our frequency | a stranger of the same mode on the channel: how many of our packets it costs, and that our boards count its packets as nothing of theirs. Needs no port |
| The T-Echo, ported | 2.1 on it: the L76K's NMEA lag behind its PPS, on an nRF52840 | the L1's own lag measured directly, not guessed from the Heltec's, whose UART and processing differ |
| | 2.3 and 2.4 between it and the Heltec | two boards with PPS judge each other: the Heltec's own lateness, which nothing above can see |
| | stage 6 on it | the walker's board as it would be carried: small, on its own battery |

The T-Echo's port is its own piece of work (T-boards): Zephyr 4.5 has no board file for it, and
our Zephyr side stamps no PPS edge yet, the L1 having none [code: `zephyr-ws/zephyr/boards`,
`firmware/zephyr/src`]. It is worth doing after stage 2 on the first two boards, not before.
