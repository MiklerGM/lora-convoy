# Vendored libraries

## android-aidl-lib-5.4.aar

OsmAnd's client library for its AIDL V2 API (`net.osmand.aidlapi`); its source is `OsmAnd-api/` in
OsmAnd's repository.

- From OsmAnd's own Ivy repository, not on Maven Central:
  `https://builder.osmand.net/ivy/net.osmand/android-aidl-lib/5.4/android-aidl-lib-5.4.aar`
- Downloaded 2026-10-22.
- SHA-256 `9f5419af2d4b8133460af370e9ed0ddaa648bf672c76ce82e04d41fe313afdec`, the same file MeshAnd
  vendors.

Kept in the repository because OsmAnd rebuilds its revision folders in place: a download at build
time would not give the same build twice. To update, download a newer revision from the same path,
replace the file, its name in `../build.gradle` and the hash above.
