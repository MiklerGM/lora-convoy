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
| Group size | designed for 10 riders; up to 20, with freshness degrading in proportion (twice the riders, at most twice the interval), never a breakdown | both numbers are assumptions to confirm (question 1) |
| Position freshness | the age of the fix **shown on the receiver's map**, at a stated percentile (e.g. 95 % of the time under N s) | not the transmission interval: losses, relays and phone or Bluetooth outages count too. At 60 km/h a 10 s old point is already 167 m behind. For comparison: DMD sent every 1.5–6 s, tuned Meshtastic 5–10 s ([03](03-alternatives.md)) |
| Separate statuses | "radio last heard", "GPS fix age" and "help acknowledged" are three different facts | a fresh heartbeat proves neither a fresh position nor delivered help |
| Help delivery | "need help" acknowledged within N s at the 95th percentile | N still to set (question 2); [06](06-own-protocol.md) sizes the slots that carry it |
| No GPS fix | the rider's own app says there is no fix; the others see "no position" or an aged last fix, never a fresh-looking one | a radio without a valid time base must not transmit into a schedule ([06](06-own-protocol.md)) |
| Range | direct radio range, roughly 1–4 km | how far a column spreads is still to confirm |
| Infrastructure | none: no repeaters, no internet | |
| Phone | Android with OsmAnd | iOS out of scope: OsmAnd's API there is different ([05](05-osmand-integration.md)) |
| Radio | off-the-shelf boards with an SX1262: Seeed Wio Tracker L1, Heltec V3/V4, LILYGO T-Deck, RAK | a GPS on the node is preferred: it keeps reporting if the phone dies. A schedule needs its PPS output, which not every board has ([07](07-experiments.md), E0) |
| Regulation | EU 868 MHz SRD rules: power and duty cycle per sub-band | [02](02-radio-basics.md); outside the EU, the national rules |
| Duty ledger | each device keeps, and can export, its transmit time per hour by category: positions, relays, routes, retries, control | a self-built device has no manufacturer's declaration to rely on |
| Privacy | only the group can read or forge its positions | |
| Interoperability with plain Meshtastic | a decision, not an option (question 4) | a yes rules out the scheduled design of [06](06-own-protocol.md) for a first version |
| Group setup | the organiser states the number of riders; slots, frame and radio mode follow from it | [06](06-own-protocol.md) |
| Route sharing | nice to have: send a route or a change to it during the ride | via points fit one packet ([06](06-own-protocol.md)) |
| Route safety | a shared route is never silently altered off-road: exact or within 2 m, never snapped, integrity-checked | [06](06-own-protocol.md) |

The radio is not a guarantee. A group still needs an agreed regroup and missing-rider procedure
for when nothing gets through: the first version in [06](06-own-protocol.md) has no path at all to
a rider beyond direct range.

## Wishes, not yet requirements

The owner's wishes so far. Each still has to be weighed against the rest; the second column says
what it would touch.

| Wish | What it touches |
|---|---|
| The node runs from its own battery, charged from the bike, and rides through the dips of starting the engine (an ESP32 with an E220 hung on them) | the choice of board, since nRF52 boards draw several times less than ESP32 ones; a test in [07](07-experiments.md), E7 |
| Riders bring the boards they have, with or without GNSS | the companion must feed the phone's position to a radio without one (MeshAnd does not); Meshtastic takes positions from the phone at most every 10 s ([03](03-alternatives.md)); in [06](06-own-protocol.md), radios without PPS take their time from two or more that have it, and a packet says where its position came from |
| The app carries the table of rules from [08](08-regions.md), without updates, picks the profile by country and hands it to the radio | the split between app and firmware, next row; each rule keeps its date, since rules change |
| The firmware does not care about countries or routes: the app decides, the radio executes | the radio still enforces its duty cycle and power itself, keeps the last confirmed profile, stays silent without one, and refuses a band its hardware is not built for |
| Routes travel through the app, downloaded or passed phone to phone before the ride; over LoRa only changes | already the rule in [06](06-own-protocol.md); the companion's features |
| Joining by invitation: the organiser shares a link or QR code, and each rider's app sets up their own board from it, or one phone sets up several boards. A board with GNSS then works without a phone, and every phone in the group sees every board | how Meshtastic works already: a channel QR carries the key, region and preset ([03](03-alternatives.md)). In [06](06-own-protocol.md) a board's slot must come from a personal invitation or from the organiser's radio, never from a hash of the board's identity ([04](04-dmd-protocol.md)). A link or a QR code forwarded through a messenger leaves the key in the chat's history; that is accepted, since the alternative is running a server, and a key per ride limits the damage. A board without a phone takes a new country's profile only if the organiser sends it over the air at a regroup, or if the board itself knows the countries |

## Non-goals, for now

- voice;
- a public, city-wide mesh;
- talking to DMD devices;
- iOS.

## Open questions for the owner

1. How many riders does a group usually have?
2. How fresh must a shown position be, and how often may it be late: is 10 s at the 95th
   percentile enough, or does it need 1–2 s? And how soon must "need help" be acknowledged?
   Two independent reviews proposed a candidate: the shown fix under about 30 s old and the map
   error under about 60 m, both at the 95th percentile, with predicted points marked and stop and
   help sent at once ([07](07-experiments.md), E9 tests it). The answer decides between the two
   paths: at 10–15 s a tuned Meshtastic with a companion may be enough.
3. How far does a column spread on a typical ride?
4. Will some riders bring plain Meshtastic radios and apps, so that they must interoperate?
5. Is the node powered by the bike, or must it run a day on its own battery?
6. Which countries will the group ride in? Decision (EU) 2025/105 does not apply outside the EU,
   so the band and power there depend on national rules. What is known so far, and the gaps, is in
   [08](08-regions.md); Morocco, for one, allows 869.40–869.65 MHz at 500 mW.
7. Does the organiser ride with the group, and where in it? In [06](06-own-protocol.md) the
   organiser's radio hands out spare slots.
