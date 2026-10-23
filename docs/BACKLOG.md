# Backlog

Tickets: open decisions, and features that no milestone has taken yet. Each says what is open, the
options known so far, what would settle it, and which milestone needs it; any of them can become a
GitHub issue as it stands. [ROADMAP](ROADMAP.md) names the tickets each milestone waits for.

The owner's questions 1–7 in [01](01-goals.md) stay there. Questions 1 and 2, group size and
freshness, set the default radio mode in M5.

## For the owner

### T-licence: the licence, and publishing

Apache-2.0, as dmd2-lora-bridge, or GPL-3.0, which reusing MeshAnd's or Meshtastic's code would
require. The owner is open to GPL-3.0 if we take MeshAnd's code as well as the bridge's
(2026-10-22). Settled the first time code is copied from a GPL project, and before the repository is
published.

### T-app-id: the app's name and package

Provisionally "LoRa Convoy", `io.github.miklergm.convoy`. Free to change until a build is handed
to anyone, since Android ties installs and updates to the package.

## Protocol

### T-phy: radio modes, sync word, preamble

v0 uses SF7/250 at 869.525 MHz, a sync word of our own and an 8-symbol preamble. Open: the default
modes per group size ([06](06-own-protocol.md), Sizing), the sync word's value and whether EBYTE
and other chips leave it alone, a shorter preamble. Settled by E2 (range) and E10 (occupancy) in
[07](07-experiments.md), and a hardware check of the sync word: in dmd2-lora-bridge one wrong
sync symbol cost most packets. M5 went without it: v1 keeps v0's mode for every group, and the
invitation does not carry one yet. Needed by M7.

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
thousand restarts. M6 kept the session; now that every board knows the frame, its number could
replace it.

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
itself (`lag=`). Still open: the lag on the L1 (E0), the fix stamped by the UART's interrupt rather
than by the parser, time from packets heard without a fix, and a clock disciplined against PPS for
longer holdover. A fix that came after the next PPS edge would be paired with it, a second out: the
lag must stay well under a second (E0), or the interrupt must count edges.

### T-join: late joiners and roster changes

Spare slots and their single allocator, a roster change at an activation epoch, what happens when
the organiser's radio is lost ([06](06-own-protocol.md), open questions). **M6, for now:** each
rider's slot comes with their invitation, so a late joiner finds it waiting; a board sits out the
frame running when its time comes, and counts a clash, another radio in its slot, without acting
on it. Still open: what a clash makes a board do, more riders than the invitations, and a use for
the spare slot. Needed by M7.

### T-neighbours: two groups on one channel

A frame offset, a slot order or a frequency derived from the group key. **M6:** a phase for each
frame from the group key, under one spacing, with a spare slot keeping frames apart. Two groups of
ten on one channel lose a third of their packets to each other, spread evenly over riders and
frames ([PROTOCOL](PROTOCOL.md)). Groups apart by frequency remain T-group-freq.

### T-cadence: sending less when stopped

A fixed frame with skipped slots, or a variable frame. After M6.

### T-help: "need help" and its acknowledgement

A flag plus a follow-up, or a spare slot; who acknowledges, and how the sender learns it was heard.
[01](01-goals.md) asks for an acknowledgement within N seconds. **v2, for now:** the flag in every
packet of the caller, and a byte in every packet that answers one caller heard, each in turn when
several call ([PROTOCOL](PROTOCOL.md)): among twenty riders the first answer comes in 1.3 s and the
last in 5.3 s, at the median. Still open: N itself (question 2 in [01](01-goals.md)); a call from
the board's button when the phone is gone; a call that outlives a restart of the board; a call heard
only through a relay (T-relay).

### T-relay: work out relaying

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

Trails, a widget listing riders nearest first, a predicted point marked as prediction, colours
riders keep between rides ([05](05-osmand-integration.md), Implications). After M4.

### T-osmand-opengl: riders blink on OsmAnd's OpenGL engine

Each update hides the whole layer while OsmAnd rebuilds it ([05](05-osmand-integration.md)). On an
emulator, at one update a second, the riders were missing from 40–45 % of screenshots; on the
Version 1 engine, from none. Measure it on a phone first. Then: update less often, or only riders
who moved; advise the Version 1 engine; or propose to OsmAnd that its layer move markers instead of
rebuilding them. Needed by M4.

### T-osmand-upstream: fixes to offer OsmAnd

Two found while building M1 and M2 ([05](05-osmand-integration.md)): `navigateGpx` throws in the
caller unless `passWholeRoute` is set, a one-line fix in its library; and T-osmand-opengl. Report
them, or send a pull request, once the owner decides to.

### T-osmand-perm: OsmAnd's permission groups

OsmAnd's `develop` gates each call by a permission group and lets an app ask for groups in a
dialog (`net.osmand.aidl.REQUEST_PERMISSIONS`); 5.4 has neither ([05](05-osmand-integration.md)).
Support both once a release has the groups.

### T-invite: invitations as QR codes, riders' names, several boards

M5's invitation is a string sent as text. Open: a QR code the organiser shows and the rider scans,
and a link a messenger makes tappable, which wants a domain or app links; riders' names and colours
in the invitation, so the map stops saying "Rider 2"; one phone setting up several boards, such as a
pillion's or loaners, without joining each slot itself; a new key for the group when a rider leaves.
Needed by M7.

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
Needed by M7.

## Firmware and boards

### T-boards: more boards

The Heltec V3 (no GNSS), the RAK4631 with its GNSS, the T-Beam, the ThinkNode M1, the Wio Tracker
L1 Pro 1W (its radio's own power switch). After M4.

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
