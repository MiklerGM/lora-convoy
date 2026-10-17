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

> **Review (Codex, 2026-10-17):** [docs][code] Encryption alone does not authenticate
> positions: legacy channel AES-CTR detects no forgery, and any shared-key holder can
> impersonate a rider. Current firmware adds signed broadcasts and optional channel AEAD;
> their compatibility and airtime costs belong in the comparison. See the
> [encryption documentation](https://meshtastic.org/docs/overview/encryption/) and
> [`src/mesh/Router.cpp`, commit `fad52bba0db68aff926aa02a55f4d2c4989f68dd`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/mesh/Router.cpp).

### Radio in Europe

See [02](02-radio-basics.md). The point that matters: `EU_868` has a single 250 kHz slot, 869.525
MHz, shared with every public LongFast node around [docs][code]. A group cannot move to another
frequency inside that region, only to another preset or key. `EU_866` multiplies all position
intervals by 10 in firmware (`PROFILE_LITE`, `positionThrottle = 10`) [code], which rules it out.

> **Review (Codex, 2026-10-17):** [code] The multiplier applies to the **periodic**
> interval; smart-position timing uses an unscaled `minimumTimeThreshold`. It does not
> multiply every position interval. Current main also offers `EU_N_868` with narrow
> presets, so the single-slot limitation is specific to `EU_868` at 250 kHz. Sources:
> [`src/mesh/Default.cpp`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/mesh/Default.cpp),
> [`src/modules/PositionModule.h`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/modules/PositionModule.h),
> [`src/modules/PositionModule.cpp`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/modules/PositionModule.cpp),
> and [`src/mesh/RadioInterface.cpp`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/mesh/RadioInterface.cpp),
> all at commit `fad52bba0db68aff926aa02a55f4d2c4989f68dd`.

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

> **Review (Codex, 2026-10-17):** [code] These notes mix a measured release with
> newer source. The installed 2.7.26 tag is commit
> [`54e0d8d0ab2ff56b3a9ce967e53f79e49af560fb`](https://github.com/meshtastic/firmware/blob/54e0d8d0ab2ff56b3a9ce967e53f79e49af560fb/src/modules/PositionModule.cpp);
> its `PositionModule.cpp` has no six-hour stationary floor. That floor exists in
> [current `src/modules/PositionModule.cpp`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/modules/PositionModule.cpp)
> at `fad52bba0db68aff926aa02a55f4d2c4989f68dd`. Pin the intended fleet release before
> treating the tuning recipe as one tested configuration (inference).

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

> **Review (Codex, 2026-10-17):** [code][docs] `TRACKER` sleeps after sending only
> when `power.is_power_saving` is enabled; it can maintain BLE and receive while awake.
> Excluding that role unconditionally loses a usable alternative. See
> [`src/modules/PositionModule.cpp`, commit `54e0d8d0ab2ff56b3a9ce967e53f79e49af560fb`](https://github.com/meshtastic/firmware/blob/54e0d8d0ab2ff56b3a9ce967e53f79e49af560fb/src/modules/PositionModule.cpp)
> and the [role/power-saving table](https://meshtastic.org/docs/configuration/radio/device/).

Capacity at 25 % load [calc]:

| Preset | One relay per packet | No relays (all Client Mute) |
|---|---|---|
| ShortFast | 2.2 positions/s: 20 riders every ~9 s | 4.3/s: every ~4.6 s |
| MediumFast | 0.7/s: every ~31 s | 1.3/s: every ~15 s |

> **Review (Codex, 2026-10-17):** [calc][code] These are offered-airtime budgets,
> not delivered freshness guarantees: they assume exactly zero or one relay and no
> other traffic or losses (inference). Independently applying the SX126x equation in
> [02](02-radio-basics.md), with 16-symbol preamble, explicit header and CRC, gives
> 57.984 ms / 190.976 ms for 56 bytes at SF7/SF9, confirming that arithmetic.
> Current signing adds 66 bytes and optional AEAD another 12
> ([`src/mesh/Router.cpp`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/mesh/Router.cpp),
> [`src/mesh/CryptoEngine.h`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/mesh/CryptoEngine.h),
> commit `fad52bba0db68aff926aa02a55f4d2c4989f68dd`).
> Holding the existing unsigned example constant, 122/134 bytes take 106.624/114.304 ms
> at ShortFast: 20 riders with one relay need **17.1/18.3 s** at 25% offered load,
> before other traffic. The 56-byte comparison is therefore release/security-specific.

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

> **Review (Codex, 2026-10-17):** [code] Binary datagrams supply neither sender ID
> nor timestamp. Add both, plus replay/sequence handling: identical datagram payloads
> have identical packet hashes, so an unchanged fix can be suppressed as a duplicate
> (inference). Shared-key group packets do not authenticate an individual rider.
> Unlike group text, this API also accepts an explicit path; the blanket "always
> flood" statement needs that exception. Sources:
> [`src/helpers/BaseChatMesh.cpp`](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/helpers/BaseChatMesh.cpp),
> [`src/Packet.cpp`](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/Packet.cpp),
> and [`docs/companion_protocol.md`](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/docs/companion_protocol.md),
> commit `81dbae592ffad40b4212bc0b37e82b38da08f4bc`.

- Radio presets come from the project's server; Europe's is **EU/UK (Narrow): 869.618 MHz, SF8,
  62.5 kHz, CR 4/8**. A 37-byte datagram there takes about 0.43 s on air [calc]. With no
  repeaters nothing is relayed, so 10 riders every 30 s fill about 15 % of the channel.

> **Review (Codex, 2026-10-17):** [code][calc] Current MeshCore uses a **32-symbol**
> preamble at SF8, not 16
> ([`src/helpers/radiolib/RadioLibWrappers.h`, commit `81dbae592ffad40b4212bc0b37e82b38da08f4bc`](https://github.com/meshcore-dev/MeshCore/blob/81dbae592ffad40b4212bc0b37e82b38da08f4bc/src/helpers/radiolib/RadioLibWrappers.h)).
> For 37 bytes, explicit header and CRC: `Tsym=4.096 ms`,
> `Npayload=8+ceil(308/32)*8=88`, so `(32+4.25+88)*4.096=508.928 ms`.
> Ten senders every 30 s offer **17.0%** load before relays/other traffic.
> The [official preset API](https://api.meshcore.nz/api/v1/config) still lists the
> stated EU/UK modem settings; presets are suggestions, not a firmware restriction
> ([docs](https://docs.meshcore.io/radio_presets/)).

- Apps: the official one; [MeshCore Open](https://github.com/zjs81/meshcore-open), open source
  (Flutter); the [MeshCore-Solo](https://github.com/MarekZegare4/MeshCore-Solo) firmware fork with
  live location sharing.

**For a moving group without infrastructure**, the missing relay through other riders is the
deciding weakness: only riders in direct range see each other, unless one carries a separate node
running repeater firmware. On the other hand, it is careful with airtime, and its protocol suits an
app of our own.

> **Review (Codex, 2026-10-17):** [code] That limitation describes stock MeshCore.
> The already-listed **MeshCore-Solo** fork has optional relaying on the companion's
> own radio (`client_repeat`, off until enabled), alongside movement/heartbeat-gated
> live position sharing. A separate repeater radio is therefore not the only option.
> See [`examples/companion_radio/MyMesh.cpp`](https://github.com/MarekZegare4/MeshCore-Solo/blob/1f47fc56cdf1398c85b144faf21937ad04e4c46e/examples/companion_radio/MyMesh.cpp)
> and [`examples/companion_radio/ui-core/LiveShareEngine.h`](https://github.com/MarekZegare4/MeshCore-Solo/blob/1f47fc56cdf1398c85b144faf21937ad04e4c46e/examples/companion_radio/ui-core/LiveShareEngine.h),
> commit `1f47fc56cdf1398c85b144faf21937ad04e4c46e`. Include this fork in a
> comparison trial before ruling MeshCore out for topology alone (inference).

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

> **Review (Codex, 2026-10-17):** [code] Sideband's periodic telemetry targets a
> **collector**, rather than broadcasting to every rider; its UI minimum is five
> minutes and its default send interval is twelve hours. A collector can return the
> collected records, which adds a distribution step. See
> [`sbapp/ui/telemetry.py`](https://github.com/markqvist/Sideband/blob/ef572b58e88ff4e4326d51778336922a53b2a03f/sbapp/ui/telemetry.py)
> and [`sbapp/sideband/core.py`](https://github.com/markqvist/Sideband/blob/ef572b58e88ff4e4326d51778336922a53b2a03f/sbapp/sideband/core.py),
> commit `ef572b58e88ff4e4326d51778336922a53b2a03f`. Reticulum's base headers are
> 19/35 bytes, not an unavoidable signature on every packet
> ([`RNS/Reticulum.py`, commit `e40191b3d193b46b7f2d8a44424a594cd758839b`](https://github.com/markqvist/Reticulum/blob/e40191b3d193b46b7f2d8a44424a594cd758839b/RNS/Reticulum.py)).
> Distinguish stock Sideband's fit from a custom Reticulum application (inference).

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

> **Review (Codex, 2026-10-17):** [docs] Product evidence can be stronger here:
> [SpecFive's own guide index](https://specfive.com/pages/instructions) lists a
> Meshtastic guide for Trace, and [GroupTrack's site](https://www.grouptrack.org/)
> claims updates every few seconds. That is a vendor claim, without a stated fleet
> size or reception percentile; it is not a measured capacity result (inference).
> [TrailMesh7's tag documentation](https://trailmesh7.com/tag) identifies its firmware
> as a small **Meshtastic fork**, with peer relaying and unattended trackers; it is
> not evidence of a separate radio protocol.
> LoraText's [published landing page](https://lora.paintbrushoutdoors.com/)
> confirms that named group tracking is a roadmap item, but its source-code buttons
> currently point to GitHub's homepage; its advertised open-source licence remains
> unverified ([deployed page content](https://lora.paintbrushoutdoors.com/assets/LandingView.b2e1522a.js)).

> **Review (Codex, 2026-10-17):** [code] Missing map alternative if OsmAnd is
> negotiable: the standalone **Meshtastic ATAK plugin** connects directly by BLE/USB,
> avoiding the removed Android AIDL API. Its current auto-configuration explicitly
> sets the US region and overrides duty-cycle enforcement, so it needs changes
> before an EU trial. Sources:
> [`README.md`](https://github.com/meshtastic/pluginmeshtastic/blob/4c4766c338d68089d160242d51652aa939b08d33/README.md)
> and [`MeshtasticConfigManager.kt`](https://github.com/meshtastic/pluginmeshtastic/blob/4c4766c338d68089d160242d51652aa939b08d33/app/src/main/java/com/atakmap/android/pluginmeshtastic/meshtastic/MeshtasticConfigManager.kt),
> commit `4c4766c338d68089d160242d51652aa939b08d33`. Separately, Garmin itself
> documents up to 20 users and 13 km in its
> [Tread announcement](https://www.garmin.com/it-IT/newsroom/tread-powerswitch-e-bc-40-offroad/)
> [docs]; this does not verify the retailer's five-second cadence.

Garmin's choice of 169.4 MHz is worth noting: a lower frequency carries further at the same
power, and the EMEA band there allows 500 mW. It needs different radio hardware from ours.

> **Review (Codex, 2026-10-17):** [docs][calc] The 500 mW figure needs its accompanying
> restriction: EU non-specific SRDs at **169.400–169.475 MHz** have a **1% duty-cycle**
> limit, under Annex row 37c of
> [Commission Implementing Decision (EU) 2025/105](https://eur-lex.europa.eu/eli/dec_impl/2025/105/oj/eng/pdf).
> That is only 75 kHz of spectrum, so the 250 kHz LoRa presets discussed above do not
> fit (calculation). A Garmin product's compliance does not establish permission for
> our own waveform or every EMEA country (inference).
