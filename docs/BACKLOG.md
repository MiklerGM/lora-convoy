# Backlog

Tickets: open decisions, and features that no milestone has taken yet. Each says what is open, the
options known so far, what would settle it, and which milestone needs it; any of them can become a
GitHub issue as it stands. [ROADMAP](ROADMAP.md) names the tickets each milestone waits for.

The owner's open questions in [01](01-goals.md) stay there. Group size and freshness, its first
two, were settled on 2026-10-24: ten riders as a first reference, a setup computed when a group is
made, and freshness measured on setups the radio can achieve rather than guessed
([ARCHITECTURE](ARCHITECTURE.md), D18; T-setup).

## For the owner

### T-licence: publishing

**Licence decided, 2026-10-23:** GPL-3.0-or-later for the whole repository (ARCHITECTURE, D14).
Before the repository goes public: a history check (done 2026-10-23: no secrets, no DMD2 4.036
internals; the author's address is public already), CI that runs `make check`, a README for
strangers, and SECURITY.md saying the cipher is our own and unreviewed.

### T-app-id: the app's name and package

Provisionally "LoRa Convoy", `io.github.miklergm.convoy`. Free to change until a build is handed
to anyone, since Android ties installs and updates to the package.

## Protocol

An outside review of v3, 2026-10-24, bears on most of these: what it found, checked against the
code, and the order to settle it in are in [09](09-protocol-next.md).

The capability register, common scenarios and evidence rules are in
[10](10-protocol-comparison.md). Its topology and sensitivity suites run current v3 with ideal and
non-ideal environments; measurements on boards will calibrate those assumptions. SDK-inspired
designs and scoped Meshtastic patches remain candidates, not implemented or field-tested results.
The full simulator design and staged readiness criteria are in [11](11-simulation-model.md):
independent truth and observations, DES/radio lifecycle, movement/channel models, candidate
comparisons, calibration and replay. A first successful relay comparison is one milestone;
it does not complete the model or validate its RF assumptions.
The [DES implementation](../tools/simulator/README.md) now executes v3 and a research relay with
independent models/evaluation. Its explicit unsupported features and calibration work remain open.

### T-phy: radio modes, sync word, preamble

v0 uses SF7/250 at 869.525 MHz, a sync word of our own and an 8-symbol preamble. Open: the modes
a group's setup chooses among (T-setup; [06](06-own-protocol.md), Sizing), the sync word's value and whether EBYTE
and other chips leave it alone, a shorter preamble. Settled by E2 (range) and E10 (occupancy) in
[07](07-experiments.md), and a hardware check of the sync word: in dmd2-lora-bridge one wrong
sync symbol cost most packets. M5 went without it: v1 keeps v0's mode for every group, and the
invitation does not carry one yet. Needed by M7.

### T-setup: a group's setup, computed when it is made

Agreed [owner, 2026-10-24] ([ARCHITECTURE](ARCHITECTURE.md), D18): the organiser sets the roster,
the app computes the interval, the schedule and the radio mode from the slots, the packet's size
and airtime, the clocks' error and the radio's limits, every board gets that one setup, and one
that does not fit is refused. Nothing changes on the move. **For now:** every group gets 2 s and
one mode from the app, so an L1 without PPS is silent from the eighth rider on; the invitation
carries no mode; and `GROUP` takes setups too tight for the board's time error, the board then
silent ([09](09-protocol-next.md), fixes that need no new layout). Candidates for the comparison
runs, not profiles, are in [09](09-protocol-next.md): 5 riders every 2 s, 10 every 3 s, 20 every
6 s, on SF7/250. Settled by those runs and by E0 and E2 on the boards; recomputed once v4's size is
agreed.

### T-length: the packet's final length

The tag, a session field if the nonce needs one, an implicit header and dropping the PHY CRC, all
chosen so the length sits just under a step of airtime ([06](06-own-protocol.md), Packet sketch).
**v1, for now:** 24 bytes with an explicit header and the CRC, 30.848 ms at SF7/250; 25 and 26
bytes cost no more ([PROTOCOL](PROTOCOL.md)). Still open: the implicit header and dropping the CRC,
which want the hardware check of T-phy.

### T-nonce: how a nonce never repeats

A random or persisted session with a counter; the frame number from GPS time; or counter blocks
reserved in flash ([06](06-own-protocol.md), security rules). The frame number needs M6's
schedule, so M5 may start with a session and change. **v1, for now:** a 32-bit session drawn at
every start and before the counter wraps, with a 16-bit counter. A nonce repeats if two sessions
of one slot under one key draw the same value: about n²/2^33 for n sessions, one in 8 600 for a
thousand restarts. M6 kept the session. The frame number alone would not do: a duplicated slot,
a time set back, a changed interval or a second message in a frame repeat it; a 64-bit session with
64 bits of real entropy keeps the nonce unique, and the packet's freshness is a field of its own
([09](09-protocol-next.md)).

### T-tag: the tag's length

4 or 8 bytes, with the forgery risk and key lifetime NIST SP 800-38C (Appendix B) asks for below
64 bits. **v1, for now:** 4 bytes. One forgery in 2^32 tries: four years of a transmitter sending
nothing else, for a position of random bytes; a key lives one ride. Still to do: the budget written
out as Appendix B asks, and an outside review of the cipher and nonce (ARCHITECTURE, D13) before
the first ride.

### T-fix-time: how a position carries its time

Whole GPS seconds modulo 65 536, or the fix's age at the slot in fractions of a second, which is
finer and shorter ([06](06-own-protocol.md)). M6 kept whole seconds.

### T-timing: time without a fix, and boards without PPS

Holdover, the guard's terms, NMEA timing on the L1, frame phase recovered from packets heard. E0
measures each board. **M6, for now:** PPS edges paired with the NMEA fix of their second, or NMEA
alone less a lag the board is told; 50 ppm of drift in holdover, and silence once the uncertainty
leaves the budget ([PROTOCOL](PROTOCOL.md)). A Heltec with its kit measures the L76K's NMEA lag
itself (`lag=`), and the L1 is built with its own, measured ([07](07-experiments.md)). The L1's lag is reckoned from the satellites its fix used (07). That line did not hold on a
balcony: the L1 claimed ±50 ms and was up to 200 ms late, 22% of packets beyond its claim (07).
Not met again on 2026-11-13 (battery ruled out; the count now travels in each packet, 07).
Still open: why (16 satellites and more, or something that builds up while the L76K runs
untouched; the parser's stamp), GPS alone as the default for a board without PPS with a lag line of
its own, after three hours untouched on the balcony; what such a board may claim meanwhile, a
rider far off its slot told by its receivers, the fix stamped by the UART's interrupt rather
than by the parser, time from packets heard without a fix, and clock drift calibration for longer
holdover ([T-clock-drift](#t-clock-drift-calibrate-local-clocks)). A fix that came after the next
PPS edge would be paired with it, a second out: the lag must stay well under a second (E0), or the
interrupt must count edges. Agreed for a lost fix [owner, 2026-10-24]: the board keeps its last
position and sends only in its own slot while its time's error fits the slot's margin, then stops
and listens, with no random sends and no extra retries ([ARCHITECTURE](ARCHITECTURE.md), D19); how
long that lasts is measured, not assumed.

### T-clock-drift: calibrate local clocks

After basic E0, measure each board's local clock against GNSS, estimate its frequency error in
ppm, and correct the local-to-UTC mapping so it holds time longer without GNSS. Start with PPS on
the Heltec. On the L1, assess whether a long series of NMEA marks suffices after E0 bounds their
lag and jitter; UART and parser delays must not be mistaken for clock drift.

The clocks to calibrate [code]: the Heltec's `esp_timer` counts its 40 MHz crystal, an AT-cut one
that moves a few ppm over a ride's temperatures; the L1's uptime counts the nRF52840's 32.768 kHz
crystal (Zephyr's default, `k32src = "xtal"`, declared to 50 ppm), a tuning fork whose frequency
falls with the square of its distance from about 25 °C, typically by 0.034 ppm/°C²: some 8 ppm
slow at 10 °C, 21 ppm at 0 °C and 42 ppm at −10 °C, on top of its tolerance [docs: typical of
such crystals; the board's own to be checked]. So in the cold an L1 may drift past the 50 ppm the
schedule assumes now, calibrated or not, and a rate taken warm does not hold as the board cools.
The simulator already gives each board its crystal and takes its GNSS away (the tunnel); a fixed,
signed drift per board is what the test of the actual C needs.

Only consistent time marks may calibrate it: reject jumps and ambiguous PPS/NMEA pairing. Leave
monotonic uptime, its interval measurements and the held position's timestamp unchanged. A rate
correction must not cause duplicate or late transmissions. Keep the slot's time-error gate: the
initial uncertainty remains, and grows with the residual drift, allowing for the estimate's error
and changes in temperature and load. Without a trustworthy calibration, use the existing drift
assumption.

Settled by the actual C with known positive and negative drift, loss and return of the reference,
then E0 on each board comparing the UTC error during an outage before and after calibration, under
load and temperature changes. The result must justify the remaining uncertainty and the time a
board can keep transmitting safely; no longer holdover is promised before those measurements.

### T-join: late joiners and roster changes

Spare slots and their single allocator, a roster change at an activation epoch, what happens when
the organiser's radio is lost ([06](06-own-protocol.md), open questions). **M6, for now:** each
rider's slot comes with their invitation, so a late joiner finds it waiting; a board sits out the
frame running when its time comes. A group's roster and schedule do not change on the move for now
(T-setup). Two radios in one slot meet in every frame and never hear each
other, so each board listens in its slot one frame in 32: they hear each other as a clash within
half a minute on average, and the app tells the rider. Still open: what a clash should make a board
do by itself, more riders than the invitations, and a use for the spare slot. Needed by M7.

### T-neighbours: two groups on one channel

A frame offset, a slot order or a frequency derived from the group key. **M6:** a phase for each
frame from the group key, under one spacing, with a spare slot keeping frames apart. Two groups of
ten on one channel lose a third of their packets to each other, spread evenly over riders and
frames ([PROTOCOL](PROTOCOL.md)). Groups apart by frequency remain T-group-freq.

### T-cadence: sending less when stopped

A fixed frame with skipped slots, or a variable frame. After M6. A walker taken for stopped would
send less while walking: the stop's rule for every activity comes first ([09](09-protocol-next.md),
when a rider counts as stopped).

### T-help: "need help", and the rider's state

[01](01-goals.md) asks for a rider's state, "need help" first, to show in another rider's app
within N seconds; N is measured on the candidate setups, not guessed (T-setup). **v4** [owner,
2026-10-24] ([ARCHITECTURE](ARCHITECTURE.md), D17): one state the rider chooses, need help among
normal, please stop, need fuel and helping, said in every packet until changed, beside the facts
the board observes; no automatic answers, the interface showing that the state is going out and
how fresh each rider's radio contact is, promising no delivery; helping names no one, back to an
addressee if rides show a need; normal again after a restart or a loss of power, need help
included; no button for it on the board in the first iteration (T-board-ui). **v3, for now:** the
flag and the call's number in every packet of the caller, and a byte in every packet that answers
one call heard, naming it, each in turn when several call ([PROTOCOL](PROTOCOL.md)): among twenty
riders the first answer comes in 1.1 s and the last in 5.4 s, at the median, v3's history once v4
is built. Still open: a call heard only through a relay (T-relay).

### T-relay: work out relaying

Chosen as a direction on 2026-11-14 [owner]: passengers, 9-byte records of other riders in each
packet ([12](12-protocol-v5-proposal.md), what the owner's review changed). The rest below is the
survey it was chosen from.

**Relaying more often** [owner, 2026-11-21]. On the first walk with a relay, a T-Echo standing in
a car carried the walker and the Heltec at home to each other, but each only every 32 s: standing,
it sent every 16 s (BEAT 3), and one passenger a packet took the two in turn. An extra send costs
no other rider anything, since each has its own slot; only the board's battery and airtime, 1.9%
sending every frame at SF7/250 against the band's 10%. To try first, with no code: BEAT 0 and
PASSENGERS 2 on the standing relay, each rider then relayed every 2 s. If that helps at the edge,
the rule: a standing board sends in every frame while it carries news, a moving rider's new
position or a changed state, and at its beat otherwise. News must not include a standing rider's
heartbeat, or two standing boards that hear each other set each other off in every frame.

Version 2 in [06](06-own-protocol.md): who relays (riders someone misses, from bitmaps of who hears
whom), who owns a relay slot (by role, in turn, by the hearing graph, or piggybacked), the budget
of one rider bridging two bunches, and ages that never get younger. Compared first offline on E8's
ride logs and in the simulator: a rider bridging two bunches, a stretched column, the relay
dropping out, two candidates whose views differ. After M6.

### T-control: technical messages

Everything that is not a position: a route's intent and geometry, roster and mode changes, text.
Typed slots and their sizes, how often they come, delivery and feedback, priorities (help before a
route) ([06](06-own-protocol.md), Sharing a route). After M6.

### T-two-speed: two speeds in one frame

A fast packet every frame at SF7/250 for the riders near, and a slow slot at SF11/250 for each
rider in turn, for the far ones ([06](06-own-protocol.md), ideas). After M6.

### T-group-freq: groups apart by frequency

Where the band has room for several channels, a channel from the group key; the region profile
says how many ([06](06-own-protocol.md), ideas; [08](08-regions.md)). With T-regions.

## Routes and data

### T-route-intent: direction and segment

The app builds the GPX it hands to OsmAnd: reversed, or cut to a day's segment; with a preview of
where navigation will start on loops and out-and-back tracks ([06](06-own-protocol.md), Joining
part-way). After M2.

### T-route-lora: routes over LoRa

Exact or within 2 m off-road, snapping only on road segments each receiver verifies, an
authenticated manifest, delivery with feedback, perhaps a fountain code ([06](06-own-protocol.md)).
After M6 and T-control.

### T-hub: a hub for slow data

Routes, group setup apart from the key, rule updates, logs after a ride. Encrypted with the group
key, found by hash, no accounts and no IP logs, deleted days after the ride, with a size cap and
rate limit. Open: GPX or our binary geometry, and who runs it ([01](01-goals.md), wishes;
[06](06-own-protocol.md), Delivering it to everyone).

## App

### T-osmand-extras: more on the map

Trails, a widget listing riders nearest first, a predicted point (T-predict), colours
riders keep between rides ([05](05-osmand-integration.md), Implications). After M4.

### T-predict: a predicted position

A later task [owner, 2026-10-24]. A prediction from speed and course has a bounded horizon and
looks unlike a measured position; at a fork it may show the wrong road. Neither it nor an
animation changes a fix's time, makes a position fresh or changes a rider's observed facts
([ARCHITECTURE](ARCHITECTURE.md), D20). No field is added for it now.

### T-last-seen: the last observation, said as such

The point's menu gives when the radio last heard the rider and when its fix was taken, then
"Stopped" or a speed with nothing to say that it is the last fix's. It should read as the last
observation, "stopped at the last fix", so it does not say the rider stands now
([ARCHITECTURE](ARCHITECTURE.md), D19). Wording in the app only; no new field.

### T-osmand-opengl: riders blink on OsmAnd's OpenGL engine

Each update hides the whole layer while OsmAnd rebuilds it ([05](05-osmand-integration.md)). On an
emulator, at one update a second, the riders were missing from 40–45 % of screenshots; on the
Version 1 engine, from none. Measured again on the bench, 2026-10-23 (`bench.py map`, 40 frames
each, two riders): Version 1 showed both in 40 of 40; OpenGL in 18 and 19 of 40, each rider alone
in 19 to 31. Measure it on a phone first. Then: update less often, or only riders
who moved; advise the Version 1 engine; or propose to OsmAnd that its layer move markers instead of
rebuilding them. Needed by M4.

### T-osmand-upstream: fixes to offer OsmAnd

Two found while building M1 and M2 ([05](05-osmand-integration.md)), both still in OsmAnd's
`master` at `a68b83f` (2026-10-17):

- `navigateGpx` throws in the caller unless `passWholeRoute` is set: `NavigateGpxParams` keeps it as
  a `Boolean` that no constructor sets, and `writeToBundle` unboxes it. A one-line fix in
  `OsmAnd-api`; any app on the API meets it. **Sent, 2026-10-23:** osmandapp/OsmAnd#26774, which
  also keeps an unset value `null` through the bundle; merged the same day.
- T-osmand-opengl: on the OpenGL engine, `AidlMapLayer.onPrepareBufferImage` drops the whole layer
  (`clearAidlTileProvider`) whenever any point has moved, and builds it again; the points are gone
  until it is rebuilt. Moving the markers that moved, or building the new layer before dropping the
  old one, would keep them on screen.

OsmAnd takes pull requests under MIT. Report them, or send pull requests, once the owner decides
to; the second after a measurement on a phone.

### T-osmand-perm: OsmAnd's permission groups

OsmAnd's `develop` gates each call by a permission group and lets an app ask for groups in a
dialog (`net.osmand.aidl.REQUEST_PERMISSIONS`); 5.4 has neither ([05](05-osmand-integration.md)).
Support both once a release has the groups.

### T-invite: invitations as QR codes, riders' names, several boards

M5's invitation is a string sent as text; since M7 it names the riders, so the map says "Bob", not
"Rider 2". Open: a QR code the organiser shows and the rider scans, which wants a QR library or
encoder; a link a messenger makes tappable, which wants a domain or app links; riders' colours; one
phone setting up several boards, such as a pillion's or loaners, without joining each slot itself;
a new key for the group when a rider leaves. Needed by M7.

### T-phone-gps: boards without GNSS

The phone's position over BLE, and a packet that says where its position came from ([01](01-goals.md),
wishes). After M4.

### T-regions: countries

The table of rules from [08](08-regions.md) in the app; the profile picked by country and handed
to the board, which enforces it and keeps the last one confirmed ([01](01-goals.md), wishes).
After M7.

### T-survive: the app through a whole ride

Samsung's sleeping apps, the battery exemption, reconnects, a soak run (E7). Much of it is in
dmd2-lora-bridge already, including what its Bluetooth link learned on rides and the app's lean
`BleBoardLink` leaves out: a bond gone stale on one side, and pinning the chosen board by name.
**So far:** the app binds to OsmAnd again when OsmAnd is stopped or updated (found on the
bench: the riders stayed off its map until our service restarted); the app asks for the battery
exemption as the bridge did; Play allows that request only
for some kinds of app, which matters once it is published (T-app-id). An hour on the emulator
against a simulated board kept the link and flat memory (ROADMAP, M7); a phone over Bluetooth,
asleep in a pocket, is the test that counts. The Bluetooth client now runs on the emulator bench
against boards in netsim ([tools/bench](../tools/bench/README.md)): pairing with Android's consent,
encrypted writes, a bond kept across a board's restart, FORGET from either side, and a bond gone
stale on the phone's side, the board reset by its button while the phone was away. Android 15
pairs again by itself there, once the rider agrees; for phones that keep the stale key, as the
bridge met on rides, the app drops it after two refused sessions (BondHealth). On the bench,
2026-10-23, Android 15 asked for the first pairing's consent in a notification, not a dialog, and
the pairing waited some 30 s for it, then failed and came again with the next connection. To do:
tell the rider to look at the notifications while Android bonds with the board
(`ACTION_BOND_STATE_CHANGED`), and try whether a board associated through `CompanionDeviceManager`
brings the dialog forward, on phones.
Needed by M7.

## Firmware and boards

### T-boards: more boards

The Heltec V3 (no GNSS), the RAK4631 with its GNSS, the T-Beam, the ThinkNode M1, the Wio Tracker
L1 Pro 1W (its radio's own power switch). After M4.

### T-board-ui: a screen and a button on the board

A later task [owner, 2026-10-24]: a screen on some boards, and a button to switch the rider's chosen
state. The screen is built (below); no new use of the buttons yet, as the first iteration needs no
button for help ([ARCHITECTURE](ARCHITECTURE.md), D17).

Something useful on the screen [owner, 2026-11-16]: all three bench boards have one, and none of
them shows anything yet.

- Heltec V4: a 0.96" OLED, 128×64, SSD1306 on I2C [vendor's docs; our firmware does not touch it].
- L1: a 1.3" OLED, 128×64, SH1106 on I2C; our firmware turns it off at start, since it otherwise
  keeps the last firmware's picture (`firmware/zephyr/src/main.c`).
- T-Echo: a 1.54" e-paper, 200×200 [vendor's docs]; it keeps its picture unpowered, and a full
  redraw takes seconds, so it can only show what changes slowly. Our board file does not describe it yet.

**Built, 2026-11-21** (`firmware/src/screen.c`): the time where the phone is (ZONE), Bluetooth,
the battery (or USB); the rider's slot and state; on air or silent, and the satellites; riders heard
in the last minute, or another rider's call for help or to stop in their place. The e-paper adds
the riders known, how long since and through whom. Nothing changes by the second, so the e-paper
redraws, in part, at most once a minute. Seen right on the L1 and the T-Echo (turned 180°); the
Heltec's reads along the board, to be judged in its case. Still open: how long an OLED stays lit
(power, and burn-in over a ride), an e-paper's ghosting after many partial redraws, the button.

### T-power: power

Battery-only nRF52 boards, receive duty cycling, the GNSS in standby; bike power and the dips of an
engine start ([01](01-goals.md), wishes; E7).

### T-ledger: exporting the duty ledger

Transmit time per hour by category: positions, relays, routes, retries, control
([01](01-goals.md), requirements). **So far:** `LEDGER` gives the last hour a minute at a time, with
positions the only category yet ([PROTOCOL](PROTOCOL.md)). Still open: keeping it over a restart,
and the app saving it with the ride. Needed by M7.

### T-flasher: flashing without a toolchain

A browser flasher for the ESP32 images, as dmd2-lora-bridge planned; a UF2 file for the L1.

### T-usb: a wired link

The phone API over USB OTG as well as BLE, for tablets (dmd2-lora-bridge's M8).

## Experiments that settle tickets

From [07](07-experiments.md): E0 settles T-timing; E2 and E10 settle T-phy; E8 feeds T-relay; E12
says whether a DMD rider nearby deafens us. E6, the Meshtastic baseline in Meshtasticator, gives the
numbers this protocol has to beat; Meshtasticator is cloned when E6 runs.
