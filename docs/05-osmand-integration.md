# 05. Getting other riders onto the OsmAnd map

## OsmAnd's side: the AIDL API

OsmAnd on Android exposes an AIDL API to other apps (`OsmAnd/src/net/osmand/aidl/`) [code]:

- **map layers and points**: `addMapLayer`, `addMapPoint`, `updateMapPoint`, `removeMapPoint`;
- **map widgets** of the app's own: `addMapWidget` and related;
- **buttons in a point's context menu**: `addContextMenuButtons`.

All of these belong to the `MAP` permission group, which OsmAnd **grants to a connected app by
default** (`AidlPermissionGroup.java`, `grantedByDefault = true`). Navigation calls such as
`registerForNavUpdates` are a separate group, not granted by default.

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
