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
| Position freshness | 1–10 s | DMD sent every 1.5–6 s; Garmin's Group Ride Tracker claims 5 s; tuned Meshtastic 5–10 s ([03](03-alternatives.md)) |
| Range | direct radio range, roughly 1–4 km | how far a column spreads is still to confirm |
| Infrastructure | none: no repeaters, no internet | |
| Phone | Android with OsmAnd | iOS out of scope: OsmAnd's API there is different ([05](05-osmand-integration.md)) |
| Radio | off-the-shelf boards with an SX1262: Seeed Wio Tracker L1, Heltec V3/V4, LILYGO T-Deck, RAK | a GPS on the node is preferred: it keeps reporting if the phone dies |
| Regulation | EU 868 MHz SRD rules: power and duty cycle per sub-band | [02](02-radio-basics.md) |
| Privacy | only the group can read or forge its positions | |
| Group setup | the organiser states the number of riders; slots, frame and radio mode follow from it | [06](06-own-protocol.md) |
| Route sharing | nice to have: send a route or a change to it during the ride | via points fit one packet ([06](06-own-protocol.md)) |
| Route safety | a shared route is never silently altered off-road: exact or within 2 m, never snapped, integrity-checked | [06](06-own-protocol.md) |

## Non-goals, for now

- voice;
- a public, city-wide mesh;
- talking to DMD devices;
- iOS.

## Open questions for the owner

1. How many riders does a group usually have?
2. How fresh must a position be: is 10 s enough, or does it need 1–2 s?
3. How far does a column spread on a typical ride?
4. Will some riders bring plain Meshtastic radios and apps, so that they must interoperate?
5. Is the node powered by the bike, or must it run a day on its own battery?
