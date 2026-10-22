# 03. Alternatives

What exists for sharing positions within a group over LoRa, how well each fits
[01](01-goals.md), and what each would cost us. Sources as of 2026-10-17, pinned at the end.

## Summary

| | Meshtastic | MeshCore | Reticulum + Sideband | DMD (LoRa group rides) | Garmin Group Ride Tracker |
|---|---|---|---|---|---|
| Relays through other riders, no infrastructure | yes, managed flooding (hop limit ≤ 7) | stock: **no**, only repeaters relay; the MeshCore-Solo fork can relay on a client's own radio | through transport nodes; not checked for a moving group | TTL relay, broken (stale CRC) | not published |
| Positions out of the box | yes | stock: adverts sent by hand; MeshCore-Solo: live sharing; or our own datagrams | telemetry to a **collector**, minutes apart | yes, everyone retells everyone | yes |
| A position each every… (offered budget; 10 / 20 riders) | ShortFast, one relay: 5 s (the firmware's floor) / ~9 s; with AEAD ~5.3 / ~10.5 s, signed ~8.5 / ~17 s [calc] | whatever our app sends; ~0.51 s on air per packet | not computed | channel over 100 % at six riders [calc] | 5 s (retailer figure only) |
| API for our own companion app | the node's BLE protobuf API, with an official SDK (`org.meshtastic:sdk-core` 0.1.0); the Android app's API was removed in 2026-06 | documented BLE protocol with binary channel datagrams | RNS in Python; Sideband is non-commercial | closed to third parties since DMD2 4.036 | none |
| Time on air per position (EU) | 58 ms (ShortFast) – 191 ms (MediumFast), unsigned [calc] | ~509 ms (EU/UK Narrow, 32-symbol preamble) [calc] | heavier packets; not computed | 314–990 ms [calc] | not published |
| Licence | GPL-3.0 | MIT | Reticulum License; Sideband CC BY-NC-SA 4.0; RNode GPL-3.0 | proprietary | proprietary |
| Hardware | many boards, ours included | many boards | RNode boards | EBYTE E220 in DMD devices | Garmin zūmo / Tread only |

**Working conclusion:**

- **The reference to beat** is tuned Meshtastic with an OsmAnd companion (MeshAnd, or ours), on a
  pinned firmware release.
- **MeshCore belongs in a comparison trial:** stock MeshCore is weaker for a moving group without
  repeaters, but the MeshCore-Solo fork relays through riders.
- **ATAK with Meshtastic's plugin** is the alternative if OsmAnd turns out to be negotiable.
- **A protocol of our own** is worth it only against a measured shortfall of these
  ([06](06-own-protocol.md)).

## Meshtastic

A managed-flooding mesh: every node rebroadcasts a packet once unless it hears someone else do it
first, up to a hop limit. A channel's QR code carries the region and modem preset as well as the
key, so a group can be configured with one scan [docs].

**Encryption is not authentication.** Channels are encrypted with a shared key (AES-CTR), which
detects no forgery: anyone holding the key can impersonate any rider [docs], and even without the
key, flipping bits of a captured packet flips the same bits of its content. The `develop` branch
adds XEdDSA-signed broadcasts and an optional channel AEAD [code]. Their compatibility and their
airtime belong in any comparison (below).

**Nonces repeat now and then, with or without AEAD.** Both channel paths, CTR today and the AEAD in
`develop`, build the nonce from the sender and the packet id, and a packet id is a 10-bit counter
plus 22 random bits [code: `CryptoEngine.cpp:587,626-636`, `Router.cpp:337-356` at `fad52bb`; CTR
the same at `54e0d8d`]. At one packet every 5 s a sender repeats a nonce about 0.033 times a day,
a 6 % chance in an 8-hour day of 20 riders [calc]. A repeat leaks the XOR of two plaintexts under
either path; AEAD adds a tag and loses nothing, so it is the better setting wherever every radio
runs `develop`. Over a key's lifetime the repeats add up (about 7.6 in ten such days), so a group
changes its channel key between rides.

**The header travels in clear.** Encryption covers the payload only. The 16-byte header
(destination, sender, packet id, flags with the hop limit, a one-byte channel hash, and the last
byte of the next hop and of the relaying node) can be read by anyone in range, key or not [code:
`RadioInterface.h:36-55` at `fad52bb`]. The sender's node number comes from the radio's MAC address
and stays the same from ride to ride [code: `NodeDB.cpp:2241-2248` at `fad52bb`], so a listener
can tell which radios pass, when and how often, and recognise them later. An idea for
[06](06-own-protocol.md): keep in clear only a short group tag, and nothing that names a rider.

**Releases and `develop` differ.** Our boards run 2.7.26 (tag `54e0d8d`). Several behaviours
described below exist only in `develop` (`fad52bb`), and each is marked. A group should pin one
release for all its radios.

### Radio in Europe

See [02](02-radio-basics.md). The point that matters: in 2.7.26 the only European region is
`EU_868`, a single 250 kHz slot at 869.525 MHz, shared with every public LongFast node around. A
group cannot move to another frequency there, only to another preset or key.

`develop` adds `EU_N_868` (narrow presets in the same band) and `EU_866` (Lite presets, 865.6–867.6
MHz). In `EU_866` the **periodic** position interval is multiplied by 10 (`positionThrottle`),
while smart-position timing keeps its unscaled threshold [code], and the band's legal conditions
are narrower than the region suggests ([02](02-radio-basics.md)).

### How often a position goes out

**Defaults** (2.7.26 on our Wio Tracker L1) [bench]:

- GPS polled every 120 s;
- smart position at most every 300 s, and only after 100 m;
- a regular broadcast every 3600 s.

So a moving rider shows up every 5 minutes, and a stopped one every hour.

**Those 5 min / 1 h minimums are forced only on a default channel**: the default key (one byte
`0x01`) and a name equal to the preset's (`Channels::isDefaultChannel`, `NodeDB.cpp`) [code, 2.7.26
and `develop`]. A private channel lifts them.

**Other limits** [code, 2.7.26 and `develop`]:

- a 5 s scheduling tick;
- a smart send needs a new GPS fix, the minimum distance and the minimum interval;
- positions are withheld above **25 % channel load**, 40 % for the Tracker roles, counting
  everything the node hears ([02](02-radio-basics.md));
- for the Client role, intervals stretch once more than about 40 nodes are online;
- a position, waypoint, alert or telemetry packet from the phone is accepted at most once per 10 s
  per port, and skips the channel-load gate [code: `PhoneAPI.cpp:840-846` at `54e0d8d`]. A
  companion feeding the phone's GPS to the node cannot go faster than 10 s.

**Only in `develop`:** an unchanged position is held to a 6 h 6 min floor unless the channel
shares full precision. It does not exist in 2.7.26.

### Tuned for a group

Our reading of the source, and what [MeshAnd](05-osmand-integration.md) recommends after checking
the firmware itself [docs]:

- a private **primary** channel with **precise** location;
- ShortFast or MediumFast on every radio, whichever the local public mesh does not use. In
  `EU_868` every 250 kHz preset sits in the one slot at 869.525 MHz, so a group on the public
  preset decodes the whole public mesh: its airtime counts toward the 25 % gate, and with
  rebroadcast mode `ALL` it gets relayed. On another preset the public traffic still shares the
  air but is not decoded. In Berlin the public mesh has moved to MediumFast (the owner's
  observation);
- a node that carries only the group's channel: with the public channel too it hears far more
  nodes, and a Client stretches its intervals once more than about 40 are online (above);
- position settings:
  - GPS every 5–60 s;
  - smart position with a 10–60 s interval and 25–100 m distance;
  - a regular broadcast every 1–10 min as a heartbeat;
- the **Timestamp** position flag on, without which a stuck GPS looks like a rider standing still;
- role **Client**, or **Client Mute** when riders are close together;
- **an idea to test:** rebroadcast mode `LOCAL_ONLY`. The default, `ALL`, relays every packet heard
  with the same radio settings, other groups' and the public mesh's included, so two groups side by
  side add to each other's load. `LOCAL_ONLY` relays only traffic on the radio's own channels, and
  `KNOWN_ONLY` only that of nodes it knows [code: `config.pb.h:76-95`, `NextHopRouter.cpp:28-50` at
  `fad52bb`].

Other roles:

- **Tracker** sleeps between sends only when `power.is_power_saving` is on. Otherwise it stays
  connected and receives while awake, so it is an option, not ruled out [code, 2.7.26][docs].
  Both Tracker roles mark the node as not accepting messages (`is_unmessagable`) [code].
- **Router** and **Router Late** always rebroadcast, never cancelling a copy they heard someone
  else send [code: `FloodingRouter.cpp:117-122` at `fad52bb`]. One router relaying 20 riders at
  ShortFast holds them to a position each per 11.6 s, by its own 10 % [calc].
- **Client Hidden** sends no position.
- **TAK** sends its position once a day.

**Capacity at 25 % load** [calc]. These are offered airtime budgets, with exactly zero or one relay
and no other traffic or losses, not delivered freshness:

| Preset | One relay per packet | No relays (all Client Mute) |
|---|---|---|
| ShortFast | 2.2 positions/s: 10 riders every ~4.6 s, 20 every ~9.3 s | 4.3/s: 10 every ~2.3 s, 20 every ~4.6 s |
| MediumFast | 0.7/s: 10 every ~15 s, 20 every ~31 s | 1.3/s: 10 every ~7.6 s, 20 every ~15 s |

Stock firmware sends positions on a 5 s tick at most [code: `PositionModule.cpp:503` at
`fad52bb`], so every figure under 5 s becomes 5 s.

**Authentication changes the picture** [calc]. Sizes are from the source [code]:

| Setting | Position on air | ShortFast | One relay, each every: 10 riders | 20 riders |
|---|---|---|---|---|
| none (CTR only) | ~56 B | 58.0 ms | ~4.6 s (5 s floor) | ~9.3 s |
| AEAD (`use_aead`, +12 B, `develop`) | ~68 B | 65.7 ms | ~5.3 s | ~10.5 s |
| signed (XEdDSA, +66 B, `develop`) | ~122 B | 106.6 ms | ~8.5 s | ~17.1 s |
| signed and AEAD | ~134 B | 114.3 ms | ~9.1 s | ~18.3 s |

AEAD proves membership of the group, which is what the protocol in [06](06-own-protocol.md) would
offer, so it is the like-for-like comparison; signing proves which rider sent a packet, which
neither offers otherwise. Any comparison with Meshtastic says which release and which of the four
settings it assumes.

### What limits it in a convoy

- Random access: transmissions collide, and the firmware keeps the load under 25 %.
- Flooding: in a tight group most rebroadcasts are wasted airtime.
- A position packet is about 56 bytes on air unsigned, about 122 signed.
- In 2.7.26, the one EU slot is shared with the public mesh.
- Defaults and behaviour change between releases (2.7.15 → 2.7.26 in our case), and the Android
  app's integration API was removed in June 2026 ([05](05-osmand-integration.md)).

## MeshCore

A mesh built around fixed repeaters; MIT licence, very active [docs].

**Relaying:**

- **Stock clients do not relay**; only repeaters and room servers with `set repeat on` do
  (FAQ 5.2).
- Group text always floods through repeaters (FAQ 5.5).
- Direct messages learn a path and then travel along it. The channel-datagram API below also
  accepts an explicit path [code].

**Positions:**

- A node's position travels in its **advert** (name, key, coordinates, signed).
- Stock clients send adverts **only when the user presses the button**; repeaters every 12 h.
- Telemetry is by request or subscription.

**For an app of our own:**

- The companion protocol over BLE (a Nordic UART service) is documented in detail. Its **Send
  Channel Data Datagram** puts up to 165 bytes of binary data with a registered `data_type` into a
  group channel, so an app could send positions in its own format and at its own rate.
- A datagram carries **no sender ID and no timestamp**: the app must add both, plus sequence and
  replay handling. Identical datagrams have identical packet hashes, so an unchanged fix can be
  dropped as a duplicate [code].
- A shared channel key authenticates membership, not the individual rider.

**Airtime:**

- Radio presets come from the project's server and are suggestions, not firmware limits. Europe's
  is **EU/UK (Narrow): 869.618 MHz, SF8, 62.5 kHz, CR 4/8** [docs].
- MeshCore uses a **32-symbol preamble at SF8 and below** (`preambleLengthForSF`) [code]. A 37-byte
  datagram therefore takes **508.9 ms** on air [calc].
- With no repeaters nothing is relayed, so 10 riders every 30 s offer about 17 % load.
- The companion firmware budgets its own airtime as 1/(1 + `airtime_factor`) of the time; the
  default factor of 1.0 allows 50 %, five times the EU 10 % [code: `MyMesh.cpp:917`,
  `Dispatcher.cpp:26`]. Channel-activity detection before sending is off by default, and in
  MeshCore-Solo it stays off while `client_repeat` is on unless set by hand [code:
  `NodePrefs.h:45`; MeshCore-Solo `MyMesh.cpp:302`].

**Apps and forks:**

- the official app;
- [MeshCore Open](https://github.com/zjs81/meshcore-open), open source (Flutter);
- **[MeshCore-Solo](https://github.com/MarekZegare4/MeshCore-Solo)**, a firmware fork with optional
  relaying on the companion's own radio (`client_repeat`, off until enabled) and live position
  sharing gated by movement and a heartbeat, every 30, 60, 120 or 300 s at most [code:
  `NodePrefs.h:567-570`].

**For a moving group without infrastructure**, stock MeshCore's missing relay through riders is
the deciding weakness: only riders in direct range see each other. MeshCore-Solo removes that
limitation on the same radio, so it belongs in a comparison trial before MeshCore is ruled out.
MeshCore's datagrams are lean and its protocol suits an app of our own, but its defaults are not
set for the EU: the airtime budget needs `airtime_factor` 9 for 10 %.

## Reticulum, RNode and Sideband

- **[Sideband](https://github.com/markqvist/Sideband)**: an LXMF messenger with telemetry, location
  and offline maps, on Android among others. The radio is an
  [RNode](https://github.com/markqvist/RNode_Firmware) over BLE [docs].
- **Its periodic telemetry goes to a collector, not to every rider** [code]. The minimum interval in
  its interface is five minutes and the default twelve hours. A collector can send the collected
  records back, which adds a distribution step. That is a poor fit for a group ride as shipped.
- **A custom Reticulum application** is a different matter. Reticulum's base headers are 19 or 35
  bytes, and not every packet carries a signature [code]; its fit would need its own analysis.
- **Licences** [code]:
  - Reticulum: its own licence, MIT-like plus bans on systems built to harm people and on AI
    training;
  - Sideband: **CC BY-NC-SA 4.0**, non-commercial;
  - RNode: GPL-3.0.

## Amateur radio only

MeshCom and LoRa APRS need an amateur licence, so not every rider could use them. Two more reasons
they do not fit, even for licensed riders:

- APRS shares one slow channel among beacons sent minutes apart, not a group sending every few
  seconds;
- amateur rules forbid encryption that hides a message's meaning (internationally, ITU Radio
  Regulations No. 25.2A), so a private group channel is not allowed there [docs].

## Existing products and projects for group riding

A GitHub search on 2026-10-17 found no OsmAnd and Meshtastic projects beyond the two listed here.
Two more names that came up, BeeLine and AirVu, are left out: no repository or vendor page could be
found for either [docs].

| Name | Built on | What it does | Status / notes |
|---|---|---|---|
| [MeshAnd](https://github.com/mategogiberidze/MeshAnd) | Meshtastic (official SDK) | Android app: connects to the radio over BLE, shows the team on the OsmAnd map, with trails, pins and "not heard" alerts | GPL-3.0, since 2026-09-30, written by an AI and tested on radios; freshness thresholds in minutes ([05](05-osmand-integration.md)) |
| [Meshtastic-to-OsmAnd-Bridge](https://github.com/ProPanProPaw/Meshtastic-to-OsmAnd-Bridge) | Meshtastic | bridge between the Meshtastic app and OsmAnd | uses the Meshtastic app's AIDL, removed in 2026-06 [code] |
| [Meshtastic ATAK plugin](https://github.com/meshtastic/pluginmeshtastic) | Meshtastic | ATAK on Android, connected to the radio directly over BLE or USB | its auto-configuration sets the US region and overrides duty-cycle enforcement: needs changes for the EU [code] |
| [GroupTrack](https://grouptrack.org/) | Meshtastic | free Android off-road navigation for the US; group on Meshtastic radios, SenseCAP T1000-E set up automatically | claims updates "every few seconds", with no group size or reception figure: a vendor claim, not a measurement |
| [TrailMesh7](https://trailmesh7.com/) | a small Meshtastic fork on the RAK WisMesh Tag | phone + web app, positions and messages; tags relay for each other and can act as unattended beacons | stock Meshtastic tags need reflashing [docs] |
| SpecFive Trace | Meshtastic | tracker | SpecFive's own guide index lists a Meshtastic guide for it [docs] |
| [LoRider](https://www.hackster.io/Milliam/lorider-the-meshatastic-0cffeb) | Meshtastic | DIY bike tracker on a Wio Tracker L1 | hobby project |
| [Nelos](https://nelos.app/) | MeshCore (not affiliated) | iOS and Android, encrypted groups, asks trackers for fresh fixes over LoRa, "Find mode", Safe Zone, help alert | commercial, free with a one-off Pro |
| [Garmin Group Ride Tracker](https://www.garmin.com/en-GB/p/705043/) | proprietary, **169.4 MHz** in EMEA | up to 20 users and 13 km (Garmin's own figures); 5 s updates (retailers only) | needs vehicle power; Garmin's pages give 26.98 or 29.5 dBm |
| [EnduroRoute](https://enduroroute.app/) Trail Tracker | not stated | pocket LoRa tracker, ~5 km, AES-256 | announced, not on sale |
| [LoraText](https://lora.paintbrushoutdoors.com/) | own radio | group key, positions and pins | group tracking on its roadmap; "open source" unverified, its source links lead to GitHub's homepage |
| Cardo Ride Pro, calimoto, Packt Moto | mobile data | group positions | need coverage |

**On Garmin's 169.4 MHz.** A lower frequency carries further at the same power, but the general EU
short-range allocation there, 169.400–169.475 MHz, allows 500 mW ERP with **at most 1 % duty
cycle** (Decision 2025/105, Annex row 37c), and the emission may occupy at most **50 kHz**
(EN 300 220-2, Table B.1, row D) [docs]. None of our LoRa modes fits, 62.5 kHz included, and a Garmin
product's approval says nothing about our waveform or every country.

## Sources (pinned)

| Project | Ref |
|---|---|
| Meshtastic firmware | release `v2.7.26.54e0d8d` (commit `54e0d8d0ab2ff56b3a9ce967e53f79e49af560fb`); `develop` at `fad52bba0db68aff926aa02a55f4d2c4989f68dd` |
| Meshtastic Android | `ed0a53f39805ad3370660bc72566cfc52220d417` |
| Meshtastic ATAK plugin | `4c4766c338d68089d160242d51652aa939b08d33` |
| MeshCore | `81dbae592ffad40b4212bc0b37e82b38da08f4bc` |
| MeshCore-Solo | `1f47fc56cdf1398c85b144faf21937ad04e4c46e` |
| Sideband / Reticulum | `ef572b58e88ff4e4326d51778336922a53b2a03f` / `e40191b3d193b46b7f2d8a44424a594cd758839b` |
| MeshAnd | `034e24d173c9d37ddfaf567d05aca5531651748b` |

Documents:

- [Meshtastic encryption](https://meshtastic.org/docs/overview/encryption/)
- [Meshtastic radio settings](https://meshtastic.org/docs/overview/radio-settings/)
- [Meshtastic LoRa configuration](https://meshtastic.org/docs/configuration/radio/lora/)
- [Meshtastic device roles](https://meshtastic.org/docs/configuration/radio/device/)
- [MeshCore FAQ](https://github.com/meshcore-dev/MeshCore/blob/main/docs/faq.md)
- [MeshCore preset API](https://api.meshcore.nz/api/v1/config)
- [TrailMesh7 tag](https://trailmesh7.com/tag)
- [SpecFive guides](https://specfive.com/pages/instructions)
- [Garmin Tread announcement](https://www.garmin.com/it-IT/newsroom/tread-powerswitch-e-bc-40-offroad/)
- [Commission Implementing Decision (EU) 2025/105](https://eur-lex.europa.eu/eli/dec_impl/2025/105/oj/eng/pdf)
