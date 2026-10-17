# 01. Goals and requirements

## Use case

A group of riders, mostly motorcycles, often off-road or in the mountains where there is no mobile
coverage. Each rider wants to:

- see where the others are, on the navigation map they already use (OsmAnd on Android);
- notice who has stopped, fallen behind or taken another turn;
- send a few short things: "need help", a regroup point, a line of text.

## Requirements (draft)

| | Target | Notes |
|---|---|---|
| Group size | up to 20 riders (proposed for a first version) | typical size still to confirm |
| Position freshness | the age of the fix **shown on the receiver's map**, at a stated percentile (e.g. 95 % of the time under N s) | not the transmission interval: losses, relays and phone or Bluetooth outages count too. At 60 km/h a 10 s old point is already 167 m behind. For comparison: DMD sent every 1.5–6 s, Garmin's Group Ride Tracker reportedly 5 s, tuned Meshtastic 5–10 s ([03](03-alternatives.md)) |
| Separate statuses | "radio last heard", "GPS fix age" and "help acknowledged" are three different facts | a fresh heartbeat proves neither a fresh position nor delivered help |
| Range | direct radio range, roughly 1–4 km | how far a column spreads is still to confirm |
| Infrastructure | none: no repeaters, no internet | |
| Phone | Android with OsmAnd | iOS out of scope: OsmAnd's API there is different ([05](05-osmand-integration.md)) |
| Radio | off-the-shelf boards with an SX1262: Seeed Wio Tracker L1, Heltec V3/V4, LILYGO T-Deck, RAK | a GPS on the node is preferred: it keeps reporting if the phone dies |
| Regulation | EU 868 MHz SRD rules: power and duty cycle per sub-band | [02](02-radio-basics.md) |
| Privacy | only the group can read or forge its positions | |
| Group setup | the organiser states the number of riders; slots, frame and radio mode follow from it | [06](06-own-protocol.md) |
| Route sharing | nice to have: send a route or a change to it during the ride | via points fit one packet ([06](06-own-protocol.md)) |
| Route safety | a shared route is never silently altered off-road: exact or within 2 m, never snapped, integrity-checked | [06](06-own-protocol.md) |

Review (Codex qwen, 2026-10-17): four requirements I think belong in this table, all
implied by the rest of the repository but never stated as requirements:

- **A duty-cycle ledger per device.** The table bounds power and duty cycle but never says the
  device must *prove* it stayed inside them. [07](07-experiments.md) asks for "each transmitter's
  complete duty ledger over an hour" as a test, yet a test needs something to measure, and a
  self-built device has no manufacturer's declaration to fall back on. I would make "the device
  keeps and can export a per-hour transmit ledger covering positions, relays, routes, retries and
  control" a requirement, not only a test. [inference]
- **A freshness target for "help acknowledged".** It is listed as a separate fact but carries no
  number, so nothing can be planned against it — and it is the one message where a delay is the
  difference between a flat tyre and a search. The target is needed before
  [06](06-own-protocol.md) can size the slots that carry it. [inference]
- **Behaviour with no GPS at all.** The table prefers a GPS on the node, and
  [06](06-own-protocol.md) builds its whole schedule on GPS time, but nothing covers a rider whose
  GPS never gets a fix. That rider either stays silent and looks broken, or transmits without a
  time base and collides. The open questions in [06](06-own-protocol.md) mention losing PPS; the
  requirement here should say what the rider sees. [inference]
- **Interoperability is a requirement or a non-goal, not a question.** Q4 asks whether plain
  Meshtastic riders must be supported. That answer changes the design more than anything else in
  the table: a Meshtastic-interoperable v1 cannot use the 24-byte slot packet or the TDMA frame in
  [06](06-own-protocol.md) at all. [inference]

On the Garmin figure in the freshness row: [03](03-alternatives.md) is right to call the 5 s a
retailer figure, and I would go further and drop it from this table. A benchmark has to be
comparable, and an unattributed retail number cannot be compared with our own measured percentile
at all. [inference]

The radio is not a guarantee. A group still needs an agreed regroup and missing-rider procedure
for when nothing gets through: the first version in [06](06-own-protocol.md) has no path at all to
a rider beyond direct range.

## Non-goals, for now

- voice;
- a public, city-wide mesh;
- talking to DMD devices;
- iOS.

## Open questions for the owner

1. How many riders does a group usually have?
2. How fresh must a shown position be, and how often may it be late: is 10 s at the 95th
   percentile enough, or does it need 1–2 s?
3. How far does a column spread on a typical ride?
4. Will some riders bring plain Meshtastic radios and apps, so that they must interoperate?
5. Is the node powered by the bike, or must it run a day on its own battery?

Review (Codex qwen, 2026-10-17): two more, both cheap to answer and both blocking:

6. Which countries will the group actually ride in? Power and duty cycle are harmonised by
   Decision (EU) 2025/105, but the four 865.6–867.6 MHz windows in Annex row 47b carry
   per-country notes, so the sub-band choice in [06](06-own-protocol.md) depends on the route.
   [docs]
7. Who carries the organiser's radio, and what happens when that rider is the one who stops? The
   spare-slot allocator in [06](06-own-protocol.md) is the organiser's radio, so a dead organiser
   radio means no late joiners and no roster changes for the rest of the ride. [inference]
