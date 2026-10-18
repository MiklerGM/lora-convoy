# 05. Getting other riders onto the OsmAnd map

Sources pinned: OsmAnd at `a68b83f322aa1bf76c8a98a2d76da261d59b15f7`, Meshtastic Android at
`ed0a53f39805ad3370660bc72566cfc52220d417`, MeshAnd at `034e24d173c9d37ddfaf567d05aca5531651748b`.

## OsmAnd's side: the AIDL API

OsmAnd on Android exposes an AIDL API to other apps; [OsmAnd's API
guide](https://osmand.net/docs/technical/osmand-api-sdk/) describes the supported approach
[docs][code]:

- **map layers and points**: `addMapLayer`, `addMapPoint`, `updateMapPoint`, `removeMapPoint`;
- **map widgets** of the app's own: `addMapWidget` and related;
- **buttons in a point's context menu**: `addContextMenuButtons`.

All of these belong to the `MAP` permission group, which OsmAnd grants by default
(`AidlPermissionGroup.java`, `grantedByDefault = true`). Navigation calls such as
`registerForNavUpdates` are a separate group, not granted by default.

**The rider still has to enable the app.** OsmAnd records a newly connecting app as disabled
(`getOrCreateConnectedApp` creates it with `enabled = false`), and every call checks that the app is
enabled as well as that its group is granted (`isMethodAllowed`) [code]. A companion's setup must
walk the rider through enabling it, and handle access being revoked later.

Other features need other groups. None of them is granted by default, so the rider allows each
once [code: `AidlPermissionGroup.java:22-49`]:

| Group | Calls | Needed for |
|---|---|---|
| `TRACKS_EDIT` | `importGpx`, `showGpx` | routes, and **trails**: MeshAnd draws each rider's trail as an imported GPX |
| `TRACKS_VIEW` | `getImportedGpx` | listing tracks already imported |
| `NAVIGATION` | `navigateGpx`, `navigate`, `registerForNavUpdates` | navigating to a rider or along a route |
| `SYSTEM` | `setNavDrawerItems` | an entry in OsmAnd's side menu |
| `LOCATION` | `getAppInfo`, `getMapWidgetValues` | reading OsmAnd's own values; showing a widget (`addMapWidget`) is in `MAP` |

MeshAnd uses `MAP` and the first four [code: MeshAnd `OsmAndBridge.kt:359,477,557,591,611`]. A
companion asks for each group when a feature first needs it: one that asks only for `MAP` draws
points without trails, which looks like a bug rather than a missing permission.

**Snapping a route to roads needs the V2 interface.** In the V2 interface
(`net.osmand.aidlapi.navigation.NavigateGpxParams`), `navigateGpx` takes `snapToRoad`,
`snapToRoadMode`, `snapToRoadThreshold` and `passWholeRoute`, so OsmAnd itself can snap a sparse
track to roads ([06](06-own-protocol.md), route sharing). The legacy interface
(`net.osmand.aidl.navigation.NavigateGpxParams`) has none of them. Pin the API version and check the
installed OsmAnd's [code].

**A successful call is only an accepted request.** `NavigateGpxHelper.saveAndNavigateGpx` returns
before saving, road approximation and navigation have finished [code], so a successful call does
not mean the rider is following the route. A companion keeps "received", "imported" and
"navigating" as separate states, and lets the rider approve replacing a route.

A layer point (`AMapPoint`) carries `id`, `shortName`, `fullName`, `typeName`, `layerId`, `color`,
`location`, a list of `details` strings and a `params` map.

OsmAnd does not load third-party code as plugins, so this needs a companion app. OsmAnd's own
Telegram tracker works the same way. iOS has no such API: out of scope.

## Meshtastic's side: where the positions come from

- **The Android app no longer exposes them.** Meshtastic-Android removed its AIDL API and the
  broadcast intents that came with it in PR #5586, merged 2026-06-04 ("Remove the deprecated AIDL
  API…"); `MeshService` is `exported="false"` [code]. Anything built on
  `com.geeksville.mesh.IMeshService` stopped working with newer app versions.
- **The app's TAK server is no substitute** [docs][code]:
  - it is optional and loopback-only, so a companion would have to run on the same phone;
  - it speaks mutual TLS on port 8089, with certificates from an exported data package, and
    Cursor-on-Target XML for ATAK;
  - ordinary mesh positions need the separate `isMeshToCotEnabled` option, and the conversion
    filters which nodes are passed on (`isEligibleForCot`).

  It is an interface for another product.
- **The node's own BLE API** is what every client uses (protobufs, GPL-3.0), and there is an
  **official client SDK** for it: `org.meshtastic:sdk-core` and `sdk-transport-ble`, 0.1.0, which
  MeshAnd uses rather than implementing the session itself [code]. A companion built on it does
  not depend on the official app's internals. The cost: probably one BLE connection per node, so
  during a ride the companion takes the official app's place. Experiment E4 in
  [07](07-experiments.md) checks this.
- **A companion leaves packet ids to the node** (`id = 0`). The firmware keeps an id the phone
  supplies as it is [code: `MeshService.cpp:317-318` at `fad52bb`], and packet ids also make the
  encryption nonces ([03](03-alternatives.md)).

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

Its pins are ordinary text messages on the primary channel, which its setup guide makes the
team's private channel, so they are encrypted like positions [code: MeshAnd `PinText.kt:9-11`,
`PinRepository.kt:121-133`, `docs/radio-setup.md`]. The format has no version byte; if we reuse
the idea, we add one.

Its `docs/radio-setup.md` is a good settings checklist (summarised in [03](03-alternatives.md)).

**Its freshness thresholds are tuned for walking, not riding** [code]:

- a marker turns stale after **60 minutes** without being heard;
- a GPS counts as stuck after **15 minutes**;
- the silence alert defaults to 60 minutes, and the shortest it can be set to is 15.

A node can also keep sending telemetry while its position is old. A riding companion needs
GPS-fix age kept apart from link age, and stale marks on a scale of seconds.

**It only listens.** A radio without a GPS gives MeshAnd no position, and the app sends nothing
but pins [docs]. Feeding the phone's position to a GPS-less radio would be extra work.

The other project found,
[Meshtastic-to-OsmAnd-Bridge](https://github.com/ProPanProPaw/Meshtastic-to-OsmAnd-Bridge), relies
on the Meshtastic app's AIDL (`MeshtasticAidlHelper`) [code] and so no longer works with current
app versions.

## Implications

- **For Meshtastic, start from MeshAnd** rather than from scratch: try it on our boards (E5), read
  the code, and contribute or fork. Its SDK boundary is the one to keep. Anything derived from it
  is GPL-3.0.
- **The OsmAnd side is the same whatever the radio protocol:**
  - one layer, one point per rider;
  - colour, GPS-fix age, link age and status;
  - a widget.
- **Prediction is shown as prediction.** Between positions the app could move a rider's point
  along their last speed and course, but at 60 km/h ten seconds of prediction moves it 167 m, even
  if the rider has stopped or turned [calc]. The rules:
  - show the last measured fix;
  - mark a predicted point as such, and stop predicting after a short timeout;
  - never let a prediction clear a "help" or "stale" mark.

  Smooth movement is not evidence of fresh reception.
