#!/usr/bin/env bash
# The app's bench without a phone: an Android emulator with OsmAnd~ and a fresh build of this app.
#
#   ./emulator.sh up        create the emulator once, boot it without a window, wait for it
#   ./emulator.sh install   OsmAnd~ from F-Droid (downloaded once, hash checked) and a debug build
#   ./emulator.sh map       a small map OsmAnd can route on (Liechtenstein), and the emulator there
#   ./emulator.sh route     a test route in Downloads, opened with LoRa Convoy as another app would
#   ./emulator.sh shot FILE a screenshot
#   ./emulator.sh down
#
# M1 by hand, after up and install: start OsmAnd once and skip its download wizard; run map; in
# LoRa Convoy tap Start (it says OsmAnd has not let it in); in OsmAnd, Menu > Plugins, switch
# LoRa Convoy on. Three riders appear around the map's centre; a tap on one shows its details,
# and NAVIGATION in that menu routes to it.
#
# M2: run route; in the app, Show in OsmAnd, then Navigate along it. A share (SEND) cannot be
# tested from adb: `am start` grants only the intent's data, and a share grants its stream.
#
# The image is Google APIs without the Play Store, so `adb root` works for looking inside apps.
set -euo pipefail

AVD=convoy-test
IMAGE="system-images;android-35;google_apis;x86_64"
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/lora-convoy
# OsmAnd~ 5.4.10, F-Droid's x86/x86_64 build: version codes end in 1 (armv7), 2 (x86), 3 (arm64).
OSMAND_URL=https://f-droid.org/repo/net.osmand.plus_541002.apk
OSMAND_SHA256=d33ba6f7f73f82adf3cc08fa7114fb771e714dc8deab2d11b90e87050e17633a
OSMAND_APK=${OSMAND_APK:-$CACHE/net.osmand.plus_541002.apk}
# OsmAnd rebuilds its maps every month, so this one is not pinned by hash: scenery, not code.
MAP_URL="https://download.osmand.net/download?standard=yes&file=Liechtenstein_europe_2.obf.zip"
MAP=$CACHE/Liechtenstein_europe_2.obf
OSMAND_FILES=/storage/emulated/0/Android/data/net.osmand.plus/files
HERE=$(cd "$(dirname "$0")" && pwd)

booted() { [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; }

case "${1:-}" in
up)
    if ! "$SDK/emulator/emulator" -list-avds | grep -qx "$AVD"; then
        echo no | "$SDK/cmdline-tools/latest/bin/avdmanager" create avd -n "$AVD" -k "$IMAGE" -d medium_phone
        sed -i 's/^disk.dataPartition.size=.*/disk.dataPartition.size=4G/; s/^hw.ramSize=.*/hw.ramSize=3G/' \
            "$HOME/.android/avd/$AVD.avd/config.ini"
    fi
    if ! booted; then
        nohup "$SDK/emulator/emulator" -avd "$AVD" -no-window -no-audio -no-boot-anim -no-snapshot \
            -gpu swiftshader_indirect >"${TMPDIR:-/tmp}/$AVD.log" 2>&1 &
        adb wait-for-device
        for _ in $(seq 180); do booted && break; sleep 1; done
        booted || { echo "the emulator did not boot in 3 minutes: ${TMPDIR:-/tmp}/$AVD.log"; exit 1; }
    fi
    echo "$AVD is up"
    ;;
install)
    if [ ! -f "$OSMAND_APK" ]; then
        mkdir -p "$(dirname "$OSMAND_APK")"
        curl -fsS -o "$OSMAND_APK.part" "$OSMAND_URL"
        mv "$OSMAND_APK.part" "$OSMAND_APK"
    fi
    echo "$OSMAND_SHA256  $OSMAND_APK" | sha256sum -c --quiet
    adb install -r "$OSMAND_APK" >/dev/null
    adb shell pm grant net.osmand.plus android.permission.ACCESS_FINE_LOCATION
    (cd "$HERE" && ./gradlew --console=plain -q :app:assembleDebug)
    adb install -r "$HERE/app/build/outputs/apk/debug/app-debug.apk" >/dev/null
    echo "OsmAnd~ and LoRa Convoy installed"
    ;;
map)
    if [ ! -f "$MAP" ]; then
        mkdir -p "$CACHE"
        curl -fsSL -o "$MAP.zip" "$MAP_URL"
        python3 -I -m zipfile -e "$MAP.zip" "$CACHE"
        rm "$MAP.zip"
    fi
    adb root >/dev/null
    adb wait-for-device
    adb shell test -d "$OSMAND_FILES" || { echo "start OsmAnd once first: it makes its folder"; exit 1; }
    adb push "$MAP" "$OSMAND_FILES/" >/dev/null
    adb shell chown "$(adb shell stat -c %U:%G "$OSMAND_FILES" | tr -d '\r')" "$OSMAND_FILES/$(basename "$MAP")"
    adb shell am force-stop net.osmand.plus # it reads its maps when it starts
    adb emu geo fix 9.5215 47.1410 >/dev/null # Vaduz
    echo "map in place, and the emulator is in Vaduz; start OsmAnd again"
    ;;
route)
    gpx=$(mktemp --suffix=.gpx)
    cat >"$gpx" <<'GPX'
<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="lora-convoy emulator.sh" xmlns="http://www.topografix.com/GPX/1/1">
  <metadata><name>Vaduz to Schaan</name></metadata>
  <trk><name>Vaduz to Schaan</name><trkseg>
    <trkpt lat="47.1380" lon="9.5225"/><trkpt lat="47.1410" lon="9.5212"/><trkpt lat="47.1440" lon="9.5195"/>
    <trkpt lat="47.1470" lon="9.5175"/><trkpt lat="47.1500" lon="9.5155"/><trkpt lat="47.1530" lon="9.5135"/>
    <trkpt lat="47.1560" lon="9.5115"/><trkpt lat="47.1590" lon="9.5100"/><trkpt lat="47.1620" lon="9.5090"/>
    <trkpt lat="47.1650" lon="9.5085"/>
  </trkseg></trk>
</gpx>
GPX
    adb push "$gpx" /sdcard/Download/vaduz-schaan.gpx >/dev/null 2>&1
    rm "$gpx"
    for _ in $(seq 20); do # MediaStore lists a pushed file a moment later
        id=$(adb shell content query --uri content://media/external/file --projection _id \
            --where "_display_name=\'vaduz-schaan.gpx\'" | sed -n 's/.*_id=\([0-9]*\).*/\1/p' | head -1)
        [ -n "$id" ] && break
        sleep 0.5
    done
    [ -n "$id" ] || { echo "MediaStore does not list the route"; exit 1; }
    adb shell am start -a android.intent.action.VIEW -d "content://media/external/file/$id" \
        -t application/octet-stream --grant-read-uri-permission -n io.github.miklergm.convoy/.RouteActivity >/dev/null
    echo "route opened with LoRa Convoy"
    ;;
shot)
    adb exec-out screencap -p >"${2:?where to}"
    ;;
down)
    adb emu kill
    ;;
*)
    sed -n '2,19p' "$0"
    exit 2
    ;;
esac
