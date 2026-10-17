# 05. Getting other riders onto the OsmAnd map

## OsmAnd's side: the AIDL API

OsmAnd on Android exposes an AIDL API to other apps (`OsmAnd/src/net/osmand/aidl/`) [code]:

- **map layers and points**: `addMapLayer`, `addMapPoint`, `updateMapPoint`, `removeMapPoint`;
- **map widgets** of the app's own: `addMapWidget` and related;
- **buttons in a point's context menu**: `addContextMenuButtons`.

All of these belong to the `MAP` permission group, which OsmAnd **grants to a connected app by
default** (`AidlPermissionGroup.java`, `grantedByDefault = true`). Navigation calls such as
`registerForNavUpdates` are a separate group, not granted by default.

> **Review (Codex, 2026-10-17):** [code] Default group permissions do not enable the
> connection itself: OsmAnd creates a new connected app with `enabled=false`, and checks
> both enablement and method permission. Include this initial user action and revoked-access
> handling in the companion's setup. Source: OsmAnd commit
> `a68b83f322aa1bf76c8a98a2d76da261d59b15f7`,
> [`OsmAnd/src/net/osmand/aidl/OsmandAidlApi.java`](https://github.com/osmandapp/OsmAnd/blob/a68b83f322aa1bf76c8a98a2d76da261d59b15f7/OsmAnd/src/net/osmand/aidl/OsmandAidlApi.java)
> (`getOrCreateConnectedApp`, `isMethodAllowed`).

For routes, other groups are needed, and neither is granted by default, so the rider allows each
once [code]:

- `importGpx`, `showGpx`: the `TRACKS_EDIT` group;
- `navigateGpx`: the `NAVIGATION` group. Its parameters (`NavigateGpxParams`) include `snapToRoad`,
  `snapToRoadMode`, `snapToRoadThreshold` and `passWholeRoute`, so a sparse track can be snapped to
  roads by OsmAnd itself ([06](06-own-protocol.md), route sharing).

> **Review (Codex, 2026-10-17):** [code] These snapping and `passWholeRoute` fields belong
> to **AIDL V2**, `net.osmand.aidlapi.navigation.NavigateGpxParams`; the legacy
> `net.osmand.aidl.navigation.NavigateGpxParams` has none of them. Pin the API artifact and
> check the installed OsmAnd version, rather than assuming both interfaces support this.
> Sources: OsmAnd commit `a68b83f322aa1bf76c8a98a2d76da261d59b15f7`,
> [`OsmAnd-api/src/net/osmand/aidlapi/navigation/NavigateGpxParams.java`](https://github.com/osmandapp/OsmAnd/blob/a68b83f322aa1bf76c8a98a2d76da261d59b15f7/OsmAnd-api/src/net/osmand/aidlapi/navigation/NavigateGpxParams.java),
> [`OsmAnd/src/net/osmand/aidl/navigation/NavigateGpxParams.java`](https://github.com/osmandapp/OsmAnd/blob/a68b83f322aa1bf76c8a98a2d76da261d59b15f7/OsmAnd/src/net/osmand/aidl/navigation/NavigateGpxParams.java).
> [code] `NavigateGpxHelper.saveAndNavigateGpx` returns success before asynchronous saving,
> road approximation and navigation finish; it is an accepted request, not proof the rider is
> following the received route. Keep received, imported and navigating statuses distinct
> (inference), and let the rider approve route replacement. Source:
> [`OsmAnd/src/net/osmand/plus/helpers/NavigateGpxHelper.java`](https://github.com/osmandapp/OsmAnd/blob/a68b83f322aa1bf76c8a98a2d76da261d59b15f7/OsmAnd/src/net/osmand/plus/helpers/NavigateGpxHelper.java)
> at the same commit. [docs] The supported external-app approach is documented in
> [OsmAnd's API guide](https://osmand.net/docs/technical/osmand-api-sdk/).

A layer point (`AMapPoint`) carries `id`, `shortName`, `fullName`, `typeName`, `layerId`, `color`,
`location`, a list of `details` strings and a `params` map.

OsmAnd does not load third-party code as plugins, so this needs a companion app. OsmAnd's own
Telegram tracker works the same way. iOS has no such API: out of scope.

## Meshtastic's side: where the positions come from

- **The Android app no longer exposes them.** Meshtastic-Android removed its AIDL API and the
  broadcast intents that came with it in PR #5586, merged 2026-06-04 ("Remove the deprecated AIDL
  API…"); `MeshService` is now `exported="false"` [code]. Anything built on
  `com.geeksville.mesh.IMeshService` stopped working with newer app versions.
- **The app runs a TAK server instead** [code]: mutual TLS on port 8089, certificates from an
  exported data package, Cursor-on-Target XML for ATAK. Usable, but it is an interface for another
  product and could change as well.
- **The node's own BLE API** is what every client uses (protobufs, GPL-3.0). A companion that
  talks to the node directly does not depend on the official app's internals. The cost: probably
  one BLE connection per node, so during a ride the companion takes the official app's place.
  Experiment E4 in [07](07-experiments.md) checks this.

> **Review (Codex, 2026-10-17):** [code] The AIDL removal is confirmed by
> [PR #5586](https://github.com/meshtastic/Meshtastic-Android/pull/5586), and `MeshService`
> remains unexported at Meshtastic Android commit `ed0a53f39805ad3370660bc72566cfc52220d417`,
> [`androidApp/src/main/AndroidManifest.xml`](https://github.com/meshtastic/Meshtastic-Android/blob/ed0a53f39805ad3370660bc72566cfc52220d417/androidApp/src/main/AndroidManifest.xml).
> [docs] The [TAK feature is optional and loopback-only](https://github.com/meshtastic/Meshtastic-Android/blob/ed0a53f39805ad3370660bc72566cfc52220d417/README.md#integration):
> a companion must run on that same phone. [code] Ordinary mesh positions additionally need
> the separate `isMeshToCotEnabled` option, and the conversion filters node entries;
> merely starting the TAK server is insufficient. Sources:
> [`core/takserver/src/commonMain/kotlin/org/meshtastic/core/takserver/MeshToCotBroadcaster.kt`](https://github.com/meshtastic/Meshtastic-Android/blob/ed0a53f39805ad3370660bc72566cfc52220d417/core/takserver/src/commonMain/kotlin/org/meshtastic/core/takserver/MeshToCotBroadcaster.kt),
> [`core/takserver/src/commonMain/kotlin/org/meshtastic/core/takserver/MeshNodeCoTConversion.kt`](https://github.com/meshtastic/Meshtastic-Android/blob/ed0a53f39805ad3370660bc72566cfc52220d417/core/takserver/src/commonMain/kotlin/org/meshtastic/core/takserver/MeshNodeCoTConversion.kt),
> same commit (`isEligibleForCot`).
> There is already an official reusable client SDK: MeshAnd pins
> `org.meshtastic:sdk-core` and `sdk-transport-ble` **0.1.0**, rather than implementing the
> protobuf/BLE session itself. Reuse that boundary if extending the companion (inference).
> Source: MeshAnd commit `034e24d173c9d37ddfaf567d05aca5531651748b`,
> [`gradle/libs.versions.toml`](https://github.com/mategogiberidze/MeshAnd/blob/034e24d173c9d37ddfaf567d05aca5531651748b/gradle/libs.versions.toml),
> [`app/src/main/java/com/meshand/app/data/meshtastic/MeshtasticClient.kt`](https://github.com/mategogiberidze/MeshAnd/blob/034e24d173c9d37ddfaf567d05aca5531651748b/app/src/main/java/com/meshand/app/data/meshtastic/MeshtasticClient.kt).

## MeshAnd

[MeshAnd](https://github.com/mategogiberidze/MeshAnd) (Kotlin, GPL-3.0, since 2026-09-30) is the
architecture above, already working [docs]:

- it connects to the user's Meshtastic radio over BLE, with no Meshtastic app;
- each teammate is a coloured point in OsmAnd, and a widget and menu list them nearest first, with
  a tap to show or navigate;
- trails from 30 minutes to 6 hours;
- shared pins as plain text messages (`meshand: 41.75002,44.77124`);
- a "GPS not updating" flag and "not heard for N minutes" alerts;
- tested by its authors on a LILYGO T-Beam Supreme. Its README says the code was written entirely
  by an AI (Claude Code) and tested by people on real radios and a phone.

Its `docs/radio-setup.md` is a good settings checklist (summarised in [03](03-alternatives.md)).

> **Review (Codex, 2026-10-17):** [code] MeshAnd's current freshness policy is much slower
> than this repository's riding cadence: map markers become stale after **60 minutes** based
> on *last heard*, while the stuck-GPS threshold is **15 minutes**; silence alerts default to
> 60 minutes, with 15 minutes the shortest enabled option. A node can keep sending telemetry
> while its location is old. Use GPS-fix age separately from link age, with a seconds-scale
> stale indication for the chosen riding cadence (inference). Sources: MeshAnd commit
> `034e24d173c9d37ddfaf567d05aca5531651748b`,
> [`app/src/main/java/com/meshand/app/data/osmand/OsmAndMapper.kt`](https://github.com/mategogiberidze/MeshAnd/blob/034e24d173c9d37ddfaf567d05aca5531651748b/app/src/main/java/com/meshand/app/data/osmand/OsmAndMapper.kt),
> [`app/src/main/java/com/meshand/app/domain/GpsHealth.kt`](https://github.com/mategogiberidze/MeshAnd/blob/034e24d173c9d37ddfaf567d05aca5531651748b/app/src/main/java/com/meshand/app/domain/GpsHealth.kt),
> [`app/src/main/java/com/meshand/app/data/settings/AppSettings.kt`](https://github.com/mategogiberidze/MeshAnd/blob/034e24d173c9d37ddfaf567d05aca5531651748b/app/src/main/java/com/meshand/app/data/settings/AppSettings.kt).
> [docs] Its [radio checklist](https://github.com/mategogiberidze/MeshAnd/blob/034e24d173c9d37ddfaf567d05aca5531651748b/docs/radio-setup.md)
> also says GPS-less radios provide no map position; the app only listens apart from pins
> ([README](https://github.com/mategogiberidze/MeshAnd/blob/034e24d173c9d37ddfaf567d05aca5531651748b/README.md)).
> Phone-to-radio GPS injection is therefore extra work for GPS-less boards, not a capability
> established by this baseline (inference).

The other project found,
[Meshtastic-to-OsmAnd-Bridge](https://github.com/ProPanProPaw/Meshtastic-to-OsmAnd-Bridge), relies
on the Meshtastic app's AIDL (`MeshtasticAidlHelper`) [code] and so no longer works with current
app versions.

## Implications

- For Meshtastic, start from MeshAnd rather than from scratch: try it on our boards (E5), read the
  code, and contribute or fork. Anything derived from it is GPL-3.0.
- Whatever the radio protocol, the OsmAnd side is the same: one layer, one point per rider, with
  colour, age and status, plus a widget.
- Between positions the app can move a rider's point along their last speed and course, marked
  with the position's age, so a 10 s cadence still looks continuous. To be judged on a ride.

> **Review (Codex, 2026-10-17):** [calc] At 60 km/h, ten seconds of extrapolation moves
> the marker **167 m** (`60 / 3.6 × 10`), even if the rider actually stopped or turned.
> Show the last measured fix and label any prediction separately; stop predicting after a
> short timeout and never use it to clear a help/stale indication (design recommendation,
> inference). Smooth movement is not evidence of fresh reception.
