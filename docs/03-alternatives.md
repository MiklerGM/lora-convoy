# 03. Alternatives

What exists for sharing positions within a group over LoRa, how well each fits
[01](01-goals.md), and what each would cost us. Sources as of 2026-10-16.

## Summary

| | Meshtastic | MeshCore | Reticulum + Sideband | DMD (LoRa group rides) | Garmin Group Ride Tracker |
|---|---|---|---|---|---|
| Relays through other riders, no infrastructure | yes, managed flooding (hop limit ≤ 7) | **no**: only repeaters relay | through transport nodes; not checked for a moving group | TTL relay, broken (stale CRC) | not published |
| Positions out of the box | yes | no: clients advertise only by hand | yes, as telemetry; cadence not checked | yes, everyone retells everyone | yes |
| 20 riders, a position each every… | ~9 s (ShortFast, one relay); ~5 s with no relays [calc] | whatever our app sends; ~0.4 s on air per packet | not computed | channel full at 4–5 riders [calc] | 5 s (retailer figure) |
| API for our own companion app | the node's BLE protobuf API; the Android app's API was removed in 2026-06 | documented BLE protocol with binary channel datagrams | RNS in Python; Sideband is non-commercial | closed to third parties since DMD2 4.036 | none |
| Time on air per position (EU) | 58 ms (ShortFast) – 191 ms (MediumFast) [calc] | ~410–440 ms (EU/UK Narrow) [calc] | heavier packets; not computed | 314–990 ms [calc] | not published |
| Licence | GPL-3.0 | MIT | Reticulum License; Sideband CC BY-NC-SA 4.0; RNode GPL-3.0 | proprietary | proprietary |
| Hardware | many boards, ours included | many boards | RNode boards | EBYTE E220 in DMD devices | Garmin zūmo / Tread only |

**Working conclusion:** tuned Meshtastic with an OsmAnd companion (MeshAnd, or ours) is the
reference to beat. MeshCore is weaker for a moving group with no repeaters. A protocol of our own
is worth it only if the requirements in [01](01-goals.md) turn out stricter than tuned Meshtastic
can meet; [06](06-own-protocol.md) sketches it.

## Meshtastic

A managed-flooding mesh: every node rebroadcasts a packet once unless it hears someone else do it
first, up to a hop limit. Channels are encrypted with a shared key (AES); a channel's QR code also
carries the region and modem preset, so a group can be configured with one scan [docs].

### Radio in Europe

See [02](02-radio-basics.md). The point that matters: `EU_868` has a single 250 kHz slot, 869.525
MHz, shared with every public LongFast node around [docs][code]. A group cannot move to another
frequency inside that region, only to another preset or key. `EU_866` multiplies all position
intervals by 10 in firmware (`PROFILE_LITE`, `positionThrottle = 10`) [code], which rules it out.

### How often a position goes out

- **Defaults** (2.7.26 on our Wio Tracker L1) [bench]:
  - GPS polled every 120 s;
  - smart position at most every 300 s, and only after 100 m;
  - a regular broadcast every 3600 s.

  So a moving rider shows up every 5 minutes, and a stopped one every hour.
- Those 5 min / 1 h minimums are forced **only on a default channel**: the default key (one byte
  `0x01`) and a name equal to the preset's (`Channels::isDefaultChannel`, `NodeDB.cpp`) [code]. A
  private channel lifts them.
- Other limits in `PositionModule.cpp` and `Default.h` [code]:
  - a 5 s scheduling tick;
  - a smart send needs a new GPS fix, the minimum distance and the minimum interval;
  - positions are withheld above **25 % channel load** (40 % for the Tracker role);
  - for the Client role, intervals stretch once more than about 40 nodes are online;
  - an unchanged position is held to a 6 h 6 min floor, **unless the channel shares full
    precision**.

### Tuned for a group

Our reading of the source, and what [MeshAnd](05-osmand-integration.md) recommends after checking
the firmware itself [docs]:

- a private **primary** channel with **precise** location;
- ShortFast or MediumFast on every radio;
- position settings:
  - GPS every 5–60 s;
  - smart position with a 10–60 s interval and 25–100 m distance;
  - a regular broadcast every 1–10 min as a heartbeat;
- the **Timestamp** position flag on, without which a stuck GPS looks like a rider standing still;
- role **Client**, or **Client Mute** when riders are close together.

  Avoid the other roles:
  - **Tracker** sleeps between sends and cannot stay connected or receive;
  - **Client Hidden** sends no position;
  - **TAK** sends its position once a day.

Capacity at 25 % load [calc]:

| Preset | One relay per packet | No relays (all Client Mute) |
|---|---|---|
| ShortFast | 2.2 positions/s: 20 riders every ~9 s | 4.3/s: every ~4.6 s |
| MediumFast | 0.7/s: every ~31 s | 1.3/s: every ~15 s |

### What limits it in a convoy

- Random access: transmissions collide, and the firmware keeps the load under 25 %.
- Flooding: in a tight group most rebroadcasts are wasted airtime.
- A position packet is about 56 bytes on air.
- The one EU slot is shared with the public mesh.
- Defaults and behaviour change between releases (2.7.15 → 2.7.26 in our case), and the Android
  app's integration API was removed in June 2026 ([05](05-osmand-integration.md)).

## MeshCore

A mesh built around fixed repeaters; MIT licence, very active [docs].

- **Clients do not relay**; only repeaters and room servers with `set repeat on` do (FAQ 5.2).
  Group channels always flood through repeaters (FAQ 5.5). Direct messages learn a path and then
  travel along it.
- A node's position travels in its **advert** (name, key, coordinates, signed). Clients send
  adverts **only when the user presses the button**; repeaters every 12 h. Telemetry is by
  request or subscription.
- The companion protocol over BLE (a Nordic UART service) is documented in detail. Its **Send
  Channel Data Datagram** puts up to 165 bytes of binary data with a registered `data_type` into a
  group channel, so an app could send positions in its own format and at its own rate.
- Radio presets come from the project's server; Europe's is **EU/UK (Narrow): 869.618 MHz, SF8,
  62.5 kHz, CR 4/8**. A 37-byte datagram there takes about 0.43 s on air [calc]. With no
  repeaters nothing is relayed, so 10 riders every 30 s fill about 15 % of the channel.
- Apps: the official one; [MeshCore Open](https://github.com/zjs81/meshcore-open), open source
  (Flutter); the [MeshCore-Solo](https://github.com/MarekZegare4/MeshCore-Solo) firmware fork with
  live location sharing.

**For a moving group without infrastructure**, the missing relay through other riders is the
deciding weakness: only riders in direct range see each other, unless one carries a separate node
running repeater firmware. On the other hand, it is careful with airtime, and its protocol suits an
app of our own.

## Reticulum, RNode and Sideband

- [Sideband](https://github.com/markqvist/Sideband): an LXMF messenger with peer-to-peer telemetry
  and location sharing and offline maps, on Android among others; the radio is an
  [RNode](https://github.com/markqvist/RNode_Firmware) over BLE [docs].
- Licences [code]:
  - Reticulum: its own licence, MIT-like plus bans on systems built to harm people and on AI
    training;
  - Sideband: **CC BY-NC-SA 4.0**, non-commercial;
  - RNode: GPL-3.0.
- Not checked: how location reaches a whole group, packet sizes, cadence. Reticulum packets are
  heavier by design (16-byte destination hashes, signatures).

## Amateur radio only

MeshCom and LoRa APRS need an amateur licence, so not every rider could use them.

## Existing products and projects for group riding

| Name | Built on | What it does | Status / notes |
|---|---|---|---|
| [MeshAnd](https://github.com/mategogiberidze/MeshAnd) | Meshtastic | Android app: connects to the radio over BLE, shows the team on the OsmAnd map, with trails, pins and "not heard" alerts | GPL-3.0, since 2026-09-30, written by an AI and tested on radios ([05](05-osmand-integration.md)) |
| [Meshtastic-to-OsmAnd-Bridge](https://github.com/ProPanProPaw/Meshtastic-to-OsmAnd-Bridge) | Meshtastic | bridge between the Meshtastic app and OsmAnd | uses the Meshtastic app's AIDL, removed in 2026-06 [code] |
| [GroupTrack](https://grouptrack.org/) | Meshtastic | free Android off-road navigation for the US; group on Meshtastic radios, SenseCAP T1000-E set up automatically | cadence not stated |
| [TrailMesh7](https://trailmesh7.com/) | own firmware on RAK WisMesh Tag | phone + web app, positions and messages; tags relay for each other and can act as beacons | stock Meshtastic tags need reflashing |
| SpecFive Trace | Meshtastic | tracker | from search results only |
| [LoRider](https://www.hackster.io/Milliam/lorider-the-meshatastic-0cffeb) | Meshtastic | DIY bike tracker on a Wio Tracker L1 | hobby project |
| [Nelos](https://nelos.app/) | MeshCore (not affiliated) | iOS and Android, encrypted groups, asks trackers for fresh fixes over LoRa, "Find mode", Safe Zone, help alert | commercial, free with a one-off Pro |
| [Garmin Group Ride Tracker](https://www.garmin.com/en-GB/p/705043/) | proprietary, **169.4 MHz** in EMEA | up to 20 riders on zūmo / Tread, 5 s updates and 13 km with the antenna kit (retailer figures) | needs vehicle power; Garmin's pages give 26.98 or 29.5 dBm |
| [EnduroRoute](https://enduroroute.app/) Trail Tracker | not stated | pocket LoRa tracker, ~5 km, AES-256 | announced, not on sale |
| [LoraText](https://lora.paintbrushoutdoors.com/) | own radio | open source, group key, positions and pins | group tracking on its roadmap |
| Cardo Ride Pro, calimoto, Packt Moto | mobile data | group positions | need coverage |

Garmin's choice of 169.4 MHz is worth noting: a lower frequency carries further at the same
power, and the EMEA band there allows 500 mW. It needs different radio hardware from ours.
