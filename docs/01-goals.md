# 01. Goals and requirements

## Use case

A group on the move, motorcyclists, cyclists, hikers or a mix of them, often off-road or in the
mountains where there is no mobile coverage. Two uses, neither above the other [owner]: a leader
watching the whole group, to notice who fell behind or stopped; and a rider catching up, watching
those ahead so as not to miss a turn. Each rider wants to:

- see where the others are, on the navigation map they already use (OsmAnd on Android);
- notice who has stopped, fallen behind or taken another turn;
- send a few short things: "need help", a regroup point, a line of text.

A last position is of use however old: it stays on the map, with a clear age.

## Requirements (draft)

| | Target | Notes |
|---|---|---|
| Group size | ten riders as the first iteration's reference, not a technical maximum; a larger group gets a longer interval, never a breakdown | settled 2026-10-24 ([ARCHITECTURE](ARCHITECTURE.md), D18) |
| Position freshness | the age of the fix **shown on the receiver's map**, and the longest gap between successful updates, at a stated percentile | no number is set by guess: setups the radio can achieve are proposed and measured first ([09](09-protocol-next.md)). The transmission interval is a third measure, apart from both: losses, relays and phone or Bluetooth outages count too. At 60 km/h a 10 s old point is already 167 m behind. For comparison: DMD sent every 1.5–6 s, tuned Meshtastic 5–10 s ([03](03-alternatives.md)) |
| Separate statuses | how fresh the radio contact is, how old the position is, the movement or stop last observed, and the rider's state are different facts: "heard 2 s ago; position a minute old; stopped at the last fix" | a fresh heartbeat proves neither a fresh position nor that one's own state was received: hearing a rider does not show that the rider hears back. A last observed stop does not say the rider stands now |
| State delivery | the delay until a rider's state, "need help" first, shows in another rider's app: within N s at the 95th percentile, from the sender choosing it to its showing in the receiver's app, under the radio conditions the scenario states | N is not set: measured first on the candidate setups ([09](09-protocol-next.md)). It shows the state reached the app, not that anyone looked at the screen. [06](06-own-protocol.md) sizes the slots that carry it |
| No GPS fix | a lost fix is not lost time: the board keeps its last position and when it was taken, its last speed and stop shown as the last observation; the rider's own app says there is no fix, and the others never see a fresh-looking position | the board sends only in its own slot while its time is good enough for it, then stops and listens; no random sends, no extra retries ([ARCHITECTURE](ARCHITECTURE.md), D19) |
| Drawn positions | an animated or predicted position never changes a fix's time, never makes a position fresh, never changes a rider's observed facts; a prediction, if any, has a bounded horizon and looks unlike a measured position | at a fork a prediction may show the wrong road; a later task ([ARCHITECTURE](ARCHITECTURE.md), D20) |
| Range | direct radio range, roughly 1–4 km | how far a column spreads is still to confirm |
| Infrastructure | during the ride, none: no repeaters, no internet | a hub for slow data is a wish (below); positions never go through it |
| Phone | Android with OsmAnd | iOS out of scope: OsmAnd's API there is different ([05](05-osmand-integration.md)) |
| Radio | off-the-shelf boards with an SX1262: Seeed Wio Tracker L1, Heltec V3/V4, LILYGO T-Deck, RAK | a GPS on the node is preferred: it keeps reporting if the phone dies. A schedule needs its PPS output, which not every board has ([07](07-experiments.md), E0) |
| Regulation | EU 868 MHz SRD rules: power and duty cycle per sub-band | [02](02-radio-basics.md); outside the EU, the national rules |
| Duty ledger | each device keeps, and can export, its transmit time per hour by category: positions, relays, routes, retries, control | a self-built device has no manufacturer's declaration to rely on |
| Privacy | only the group can read or forge its positions | |
| Interoperability with plain Meshtastic | a decision, not an option (question 4) | a yes rules out the scheduled design of [06](06-own-protocol.md) for a first version |
| Group setup | the organiser sets the roster; the app computes the interval, the schedule and the radio mode from the slots, the packet's size and airtime, the clocks' error and the radio's limits; every rider's board gets that one setup, and a setup that does not fit is refused | when the group is made; nothing changes the roster or the schedule on the move ([ARCHITECTURE](ARCHITECTURE.md), D18) |
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
| Our own hub for slow data: routes, group setup apart from the key, updates to the rules table, logs after a ride. Live positions never go through it; that is the radios' job | [06](06-own-protocol.md): a route's geometry goes to the hub encrypted with the group key and is found by its hash, so when anyone has internet only the short intent travels over LoRa, and LoRa delivery stays the fallback. Privacy by design: no accounts, no IP logs, files deleted a few days after the ride, so the hub knows neither routes nor riders. An anonymous store invites abuse, hence a size cap, a short lifetime and a rate limit. Running it costs money and upkeep |
| Joining by invitation: the organiser shares one string, shown as a QR code or sent as a link, and each rider's app sets up their own board from it, or one phone sets up several boards. A board with GNSS then works without a phone, and every phone in the group sees every board | how Meshtastic works already: a channel QR carries the key, region and preset ([03](03-alternatives.md)). In [06](06-own-protocol.md) a board's slot must come from a personal invitation or from the organiser's radio, never from a hash of the board's identity ([04](04-dmd-protocol.md)). An invitation forwarded through a messenger leaves the key in the chat's history; that is accepted, since the alternative is keeping keys on a server, which the hub below does not, and a key per ride limits the damage. A board without a phone takes a new country's profile only if the organiser sends it over the air at a regroup, or if the board itself knows the countries |

## Non-goals, for now

- voice;
- a public, city-wide mesh;
- talking to DMD devices;
- iOS.

## Open questions for the owner

Questions 1 and 2 are settled (below); the others keep their numbers.

3. How far does a column spread on a typical ride?
4. Will some riders bring plain Meshtastic radios and apps, so that they must interoperate?
5. Is the node powered by the bike, or must it run a day on its own battery?
6. Which countries will the group ride in? Decision (EU) 2025/105 does not apply outside the EU,
   so the band and power there depend on national rules. What is known so far, and the gaps, is in
   [08](08-regions.md); Morocco, for one, allows 869.40–869.65 MHz at 500 mW.
7. Does the organiser ride with the group, and where in it? In [06](06-own-protocol.md) the
   organiser's radio hands out spare slots.

## Settled questions

1. **How many riders?** Settled 2026-10-24: ten as the first iteration's reference, not a
   technical maximum; each group's setup is computed for its roster when it is made
   ([ARCHITECTURE](ARCHITECTURE.md), D18).
2. **How fresh, and how soon a state?** Settled 2026-10-24: no number is set by guess. Setups the
   radio can achieve are proposed and measured, by the shown position's age, the longest gap
   between updates and a state's delay to another rider's app ([09](09-protocol-next.md)); riders
   judging replays (E9 in [07](07-experiments.md)) is one of those measurements.
