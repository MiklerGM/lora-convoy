#!/usr/bin/env python3
"""The whole bench on one computer: phones, boards, Bluetooth and the air between them.

    bench.py up [PHONES]   build, then start PHONES+1 simulated boards (`sim serve`), each as a
                           Bluetooth board in the emulators' virtual radio (ble_board.py), and
                           PHONES emulators (2 by default) from the prepared AVD, read-only
    bench.py ride          a ride, checked step by step: each phone pairs with its own board;
                           phone 0 starts a group and the others join with their invitations; the
                           last board rides alone as a beacon; everyone hears everyone; phone 1
                           calls for help and the others' boards answer; phone 1 forgets its board
    bench.py down          stop it all
    bench.py ui PHONE list|tap TEXT     look at or tap a phone's screen, by hand
    bench.py engine PHONE opengl|v1     OsmAnd's map engine on that phone, OsmAnd restarted
    bench.py map PHONE opengl|v1 [FRAMES]   that engine set, in how many of FRAMES screenshots (30)
                                        its OsmAnd shows all the other riders, and each: the OpenGL
                                        engine's blinking, measured, with OsmAnd's version

One bench.py at a time: a second is refused while one runs, as its up or down would stop the
first one's run.

The AVD is prepared once, by android/emulator.sh up, install and map, with OsmAnd's plugin for
LoRa Convoy switched on (emulator.sh, M1). Bonds live in ~/.cache/lora-convoy/bench; a run's logs
and screenshots in ~/.cache/lora-convoy/bench/run. Standard library only, but for ble_board.py,
which runs in a venv with Bumble (requirements.txt).
"""
import base64
import fcntl
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CACHE = Path.home() / ".cache/lora-convoy/bench"
RUN = CACHE / "run"
VENV = CACHE.parent / "bench-venv"
SDK = Path(os.environ.get("ANDROID_HOME", Path.home() / "Android/Sdk"))
AVD = "convoy-test"
APP = "io.github.miklergm.convoy"
PORT = 4403
GROUP, NAMES = "Bench ride", ["Ann", "Bob", "Cid", "Dee", "Eve", "Fay"]
# Each slot's colour on the map (BoardLines.PALETTE), to find the riders in a screenshot.
COLOURS = [(0x1E, 0x88, 0xE5), (0x43, 0xA0, 0x47), (0xFB, 0x8C, 0x00), (0x8E, 0x24, 0xAA), (0x00, 0x89, 0x7B),
           (0x6D, 0x4C, 0x41)]


def phone(i: int) -> str:
    return f"emulator-{5554 + 2 * i}"


# ---- processes ---------------------------------------------------------------------

def start(name: str, *args) -> None:
    RUN.mkdir(parents=True, exist_ok=True)
    log = open(RUN / f"{name}.log", "w")
    proc = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    (RUN / f"{name}.pid").write_text(str(proc.pid))


def stop_all() -> None:
    groups = []
    for pid_file in RUN.glob("*.pid"):
        groups.append(int(pid_file.read_text()))
        pid_file.unlink()
    for sig in (signal.SIGTERM, signal.SIGKILL):  # an emulator may ignore the first
        for g in groups:
            try:
                os.killpg(g, sig)
            except ProcessLookupError:
                pass
        time.sleep(10 if sig == signal.SIGTERM and groups else 0)


def adb(serial: str, *args, check=True) -> str:
    return subprocess.run(["adb", "-s", serial, *args], capture_output=True, text=True, check=check).stdout


def port_free(port: int) -> bool:
    with socket.socket() as s:
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def wait_ports_free(phones: int, timeout=60) -> None:
    """An emulator that finds its ports taken starts on others, where adb never looks for it."""
    deadline = time.time() + timeout
    while not all(port_free(p) for i in range(phones) for p in (5554 + 2 * i, 5555 + 2 * i)):
        if time.time() > deadline:
            sys.exit("the emulator ports 5554 and up stay taken: an emulator is still running")
        time.sleep(1)


BT_CONFIG = "/data/misc/bluedroid/bt_config.conf"


def bluetooth_irk(serial: str) -> str | None:
    conf = adb(serial, "shell", "cat", BT_CONFIG, check=False)
    m = re.search(r"LE_LOCAL_KEY_IRK = (\w+)", conf)
    return m.group(1) if m else None


def bluetooth_on(serial: str, on: bool, timeout=30) -> None:
    adb(serial, "shell", "cmd", "bluetooth_manager", "enable" if on else "disable", check=False)
    deadline = time.time() + timeout
    while adb(serial, "shell", "settings", "get", "global", "bluetooth_on", check=False).strip() != ("1" if on else "0"):
        if time.time() > deadline:
            sys.exit(f"{serial}: Bluetooth would not turn {'on' if on else 'off'}")
        time.sleep(1)
    # the setting flips before the adapter is up; an app that scans meanwhile finds Bluetooth off
    adb(serial, "shell", "cmd", "bluetooth_manager", f"wait-for-state:STATE_{'ON' if on else 'OFF'}", check=False)


def own_bluetooth_keys(serial: str) -> str:
    """Copies of one AVD share its Bluetooth keys, so a board would take one phone for another; and
    the emulator's Bluetooth, made to draw new ones, draws them from a sequence that repeats. So
    each phone gets keys drawn here, written into its config. Turned off, its Bluetooth stays up
    for scans (BLE only) and later saves the config it holds over ours: its process is killed
    instead, unable to save, and Android starts it again on our file."""
    adb(serial, "root", check=False)
    deadline = time.time() + 60  # adbd restarts as root, and commands before it would fail
    while adb(serial, "shell", "id", "-u", check=False).strip() != "0":
        if time.time() > deadline:
            sys.exit(f"{serial}: adb would not run as root")
        time.sleep(1)
    bluetooth_on(serial, True)
    for _ in range(3):
        conf = adb(serial, "shell", "cat", BT_CONFIG, check=False)
        if "[Adapter]\n" not in conf:
            conf += "\n[Adapter]\n"
        irk = secrets.token_hex(16)
        for name, value in (("IRK", irk), ("IR", secrets.token_hex(16)), ("DHK", secrets.token_hex(16)),
                            ("ER", secrets.token_hex(16))):
            line = f"LE_LOCAL_KEY_{name} = {value}"
            conf, found = re.subn(rf"LE_LOCAL_KEY_{name} = \w+", line, conf)
            if not found:
                conf = conf.replace("[Adapter]\n", f"[Adapter]\n{line}\n", 1)
        staged = "/data/local/tmp/bt_config.conf"
        subprocess.run(["adb", "-s", serial, "shell", f"cat > {staged}"], input=conf, text=True, check=True)
        # com.google.android.bluetooth on Google's images, com.android.bluetooth on others
        adb(serial, "shell", f"cp {staged} {BT_CONFIG} && rm -f {BT_CONFIG[:-5]}.bak && chown bluetooth:bluetooth "
                             f"{BT_CONFIG} && chmod 660 {BT_CONFIG} && restorecon {BT_CONFIG} && "
                             "pkill -9 -f 'android[.]bluetooth$'", check=False)
        time.sleep(5)
        bluetooth_on(serial, True)
        if bluetooth_irk(serial) == irk:
            return irk
    sys.exit(f"{serial}: its Bluetooth would not keep the keys it was given")


def booted(serial: str) -> bool:
    return adb(serial, "shell", "getprop", "sys.boot_completed", check=False).strip() == "1"


def up(phones: int) -> None:
    subprocess.run(["make", "-s", "-C", str(ROOT / "firmware"), "build/host/sim"], check=True)
    subprocess.run([str(ROOT / "android/gradlew"), "--console=plain", "-q", "-p", str(ROOT / "android"),
                    ":app:assembleDebug"], check=True)
    if not (VENV / "bin/python").exists():
        subprocess.run([sys.executable, "-m", "venv", str(VENV)], check=True)
        subprocess.run([str(VENV / "bin/pip"), "install", "-q", "-r", str(ROOT / "tools/bench/requirements.txt")],
                       check=True)
    stop_all()
    for f in CACHE.glob("board-*.json"):  # fresh boards: no bonds
        f.unlink()
    wait_ports_free(phones)
    start("sim", str(ROOT / "firmware/build/host/sim"), "serve", str(phones + 1), str(PORT))
    for i in range(phones):
        start(f"phone-{i}", str(SDK / "emulator/emulator"), "-avd", AVD, "-read-only", "-port", str(5554 + 2 * i),
              "-memory", "2048", "-no-window", "-no-audio", "-no-boot-anim", "-no-snapshot",
              "-gpu", "swiftshader_indirect")
    deadline = time.time() + 600
    while not all(booted(phone(i)) for i in range(phones)):
        if time.time() > deadline:
            sys.exit("the phones did not boot in 10 minutes")
        time.sleep(5)
    # Copies of one AVD share its Bluetooth keys, so a board would take one phone for another:
    # each gets keys of its own, as a real phone has.
    irks = [own_bluetooth_keys(phone(i)) for i in range(phones)]
    print("up: each phone's Bluetooth identity key: " + ", ".join(k[:8] for k in irks))
    # the boards' Bluetooth after the phones': netsim starts with the first emulator
    for b in range(phones + 1):
        start(f"board-{b}", *board_command_line(b))
    apk = ROOT / "android/app/build/outputs/apk/debug/app-debug.apk"
    for i in range(phones):
        adb(phone(i), "install", "-r", str(apk))
        for permission in ("BLUETOOTH_SCAN", "BLUETOOTH_CONNECT", "POST_NOTIFICATIONS"):
            adb(phone(i), "shell", "pm", "grant", APP, f"android.permission.{permission}", check=False)
        adb(phone(i), "emu", "geo", "fix", "9.5215", "47.1410", check=False)
    (RUN / "phones").write_text(str(phones))
    for b in range(phones + 1):
        board_says(b, "advertising", 120)
    # netsim now and then loses the scan response, and so the name, of boards that come up together,
    # and no phone finds them: each is listened for by name, and one not heard is started again.
    for _ in range(3):
        heard = subprocess.run([str(VENV / "bin/python"), "-I", str(ROOT / "tools/bench/ble_board.py"), "listen", "12"],
                               capture_output=True, text=True).stdout.split()
        unheard = [b for b in range(phones + 1) if f"LoRaConvoy-C0FFEE{b:02X}" not in heard]
        if not unheard:
            break
        print(f"up: boards {unheard} advertise without their names; starting them again")
        for b in unheard:
            restart_board(b)
    else:
        sys.exit(f"up: boards {unheard} cannot be found by name")
    print(f"up: {phones} phones, {phones + 1} boards; logs in {RUN}")


def down() -> None:
    phones = int((RUN / "phones").read_text()) if (RUN / "phones").exists() else 0
    for i in range(phones):
        adb(phone(i), "emu", "kill", check=False)
    stop_all()
    wait_ports_free(phones)
    print("down")


# ---- a phone's screen ------------------------------------------------------------------

def screen(serial: str) -> list:
    """(text, x, y) of everything on screen that has a text or a description."""
    adb(serial, "shell", "uiautomator", "dump", "/sdcard/ui.xml", check=False)
    xml = subprocess.run(["adb", "-s", serial, "exec-out", "cat", "/sdcard/ui.xml"], capture_output=True).stdout
    try:
        root = ET.fromstring(xml)
    except ET.ParseError:
        return []
    out = []
    for n in root.iter("node"):
        text = n.get("text") or n.get("content-desc") or ""
        if text:
            x1, y1, x2, y2 = map(int, re.findall(r"\d+", n.get("bounds")))
            out.append((text, (x1 + x2) // 2, (y1 + y2) // 2))
    return out


def tap(serial: str, want: str, exact=False) -> bool:
    hits = sorted((t != want, t, x, y) for t, x, y in screen(serial) if (t == want if exact else want in t))
    if not hits:
        return False
    adb(serial, "shell", "input", "tap", str(hits[0][2]), str(hits[0][3]))
    return True


def settle(serial: str) -> None:
    """Software graphics make the launcher or System UI miss their deadlines: let them go."""
    for _ in range(3):
        if not any("responding" in t for t, _, _ in screen(serial)):
            return
        tap(serial, "Close app")
        time.sleep(2)


def wait_tap(serial: str, want: str, timeout=30) -> None:
    deadline = time.time() + timeout
    tries = 0
    while not tap(serial, want):
        if time.time() > deadline:
            raise AssertionError(f"{serial}: no {want!r} on screen")
        settle(serial)
        tries += 1
        if tries % 2 == 0:  # below the fold, perhaps: scroll down, and back up the time after
            y1, y2 = ("1600", "800") if tries % 4 == 2 else ("800", "1600")
            adb(serial, "shell", "input", "swipe", "540", y1, "540", y2, "300", check=False)
        time.sleep(2)


def main_screen(serial: str) -> str:
    # FLAG_ACTIVITY_CLEAR_TOP: the main screen in front, whatever was opened over it
    adb(serial, "shell", "am", "start", "-n", f"{APP}/.MainActivity", "-f", "0x04000000", check=False)
    time.sleep(2)
    settle(serial)
    texts = [t for t, _, _ in screen(serial)]
    return next((t for t in texts if "Stopped." in t or "Link:" in t or "Mock" in t and "\n" in t), " ".join(texts))


def wait_status(serial: str, want: str, timeout=60) -> str:
    deadline = time.time() + timeout
    while True:
        status = main_screen(serial)
        if re.search(want, status, re.DOTALL):
            return status
        if time.time() > deadline:
            raise AssertionError(f"{serial}: status never matched {want!r}; last: {status!r}")
        time.sleep(5)


def press(serial: str, items: list, want: str) -> bool:
    """Taps what reads exactly want, on a screen already read."""
    hits = [(x, y) for t, x, y in items if t == want]
    if hits:
        adb(serial, "shell", "input", "tap", str(hits[0][0]), str(hits[0][1]))
    return bool(hits)


def pair(serial: str, board: int, timeout=120) -> None:
    """Android asks the rider to consent to Just Works pairing, in a dialog or, as a rule, a
    notification, and the pairing waits some 30 s for it: each look is one screen read, and a request
    missed comes again with the app's next connection, as it would to a rider. Done once the board
    says it paired."""
    deadline = time.time() + timeout
    while "paired" not in (RUN / f"board-{board}.log").read_text():
        if time.time() > deadline:
            raise AssertionError(f"{serial}: board {board} never paired")
        items = screen(serial)
        if press(serial, items, "Pair"):  # the dialog's button
            time.sleep(2)
            continue
        if any("responding" in t for t, _, _ in items):  # software graphics: System UI or the launcher late
            press(serial, items, "Close app")
            continue
        adb(serial, "shell", "cmd", "statusbar", "expand-notifications", check=False)
        time.sleep(1)
        if not press(serial, screen(serial), "Pair & connect"):  # the notification's: the dialog next
            adb(serial, "shell", "cmd", "statusbar", "collapse", check=False)
        time.sleep(1)


def riders_on_map(serial: str, slots: list, frames=10) -> list:
    """For each of a run of screenshots, the slots whose colour shows on the map. Raw frames, not
    PNG, so the standard library reads them: a header, then RGBA rows."""
    seen = []
    for _ in range(frames):
        raw = subprocess.run(["adb", "-s", serial, "exec-out", "screencap"], capture_output=True).stdout
        w, h = int.from_bytes(raw[0:4], "little"), int.from_bytes(raw[4:8], "little")
        px = raw[len(raw) - w * h * 4:]
        found = set()
        for y in range(h // 10, h * 85 // 100, 4):  # below the status bar, above the bottom panel
            row = px[y * w * 4:(y + 1) * w * 4]
            for x in range(0, w * 4, 16):
                r, g, b = row[x], row[x + 1], row[x + 2]
                for slot in slots:
                    cr, cg, cb = COLOURS[slot % len(COLOURS)]
                    if abs(r - cr) < 18 and abs(g - cg) < 18 and abs(b - cb) < 18:
                        found.add(slot)
        seen.append(found)
        time.sleep(1)
    return seen


def board_says(board: int, text: str, timeout=30) -> None:
    deadline = time.time() + timeout
    while text not in (RUN / f"board-{board}.log").read_text():
        if time.time() > deadline:
            raise AssertionError(f"board {board} never said {text!r}")
        time.sleep(1)


def board_command_line(board: int) -> list:
    """Board 1 serves Bluetooth as the L1's Zephyr does, the others as the Heltec's NimBLE."""
    return [str(VENV / "bin/python"), "-I", str(ROOT / "tools/bench/ble_board.py"), str(board), str(PORT)] + \
        (["--l1"] if board == 1 else [])


def restart_board(board: int, forget=False) -> None:
    """Its Bluetooth side restarted, as a power cycle restarts a board's, its bonds kept as a
    board keeps them in flash; with forget, its bonds cleared too, as its button held down while
    its phone is out of reach does. Its node core in `sim serve` runs on: how the firmware comes
    back from a restart is for the bench with boards."""
    os.killpg(int((RUN / f"board-{board}.pid").read_text()), signal.SIGTERM)
    time.sleep(2)
    if forget:
        (CACHE / f"board-{board}.json").unlink(missing_ok=True)
    start(f"board-{board}", *board_command_line(board))
    board_says(board, "advertising", 60)


def board_command(board: int, *commands: str) -> list:
    with socket.create_connection(("127.0.0.1", PORT + board), timeout=3) as c:
        c.sendall("".join(f"{x}\n" for x in commands).encode())
        time.sleep(0.5)
        return [line for line in c.recv(65536).decode().splitlines() if line.startswith(("OK", "ERR"))]


# ---- OsmAnd's map, measured ----------------------------------------------------------------

OSMAND_SETTINGS = "/data/data/net.osmand.plus/shared_prefs/net.osmand.settings.xml"


def engine(serial: str, which: str) -> None:
    """OsmAnd reads use_opengl_render when it starts its map; the default is OpenGL on 64-bit
    Android 9 and later (AppInitializer)."""
    adb(serial, "root", check=False)
    deadline = time.time() + 60
    while adb(serial, "shell", "id", "-u", check=False).strip() != "0":
        if time.time() > deadline:
            sys.exit(f"{serial}: adb would not run as root")
        time.sleep(1)
    value = "true" if which == "opengl" else "false"
    adb(serial, "shell", "am", "force-stop", "net.osmand.plus", check=False)
    owner = adb(serial, "shell", "stat", "-c", "%u:%g", OSMAND_SETTINGS).strip()
    xml = adb(serial, "shell", "cat", OSMAND_SETTINGS)
    entry = f'<boolean name="use_opengl_render" value="{value}" />'
    xml = re.sub(r'<boolean name="use_opengl_render" value="\w+" />', entry, xml) if "use_opengl_render" in xml \
        else xml.replace("</map>", f"    {entry}\n</map>")
    staged = "/data/local/tmp/net.osmand.settings.xml"
    subprocess.run(["adb", "-s", serial, "shell", f"cat > {staged}"], input=xml, text=True, check=True)
    adb(serial, "shell", f"cp {staged} {OSMAND_SETTINGS} && chown {owner} {OSMAND_SETTINGS} && "
                         f"chmod 660 {OSMAND_SETTINGS} && restorecon {OSMAND_SETTINGS}")
    adb(serial, "shell", "am", "start", "-n", "net.osmand.plus/net.osmand.plus.activities.MapActivity", check=False)
    time.sleep(15)
    settle(serial)
    tap(serial, "Zoom out", exact=True)
    time.sleep(3)
    print(f"engine: phone {serial} now draws with {which}")


def osmand_version(serial: str) -> str:
    m = re.search(r"versionName=(\S+)", adb(serial, "shell", "dumpsys", "package", "net.osmand.plus"))
    return m.group(1) if m else "?"


def measure_map(i: int, which: str, frames: int) -> None:
    """The blinking, not whether riders show at all (the ride's step): the engine named and set,
    OsmAnd's version, and the share of frames with every other rider, and with each. What share is
    too little is not settled yet (T-osmand-opengl)."""
    boards = int((RUN / "phones").read_text()) + 1
    others = [s for s in range(boards) if s != i]
    engine(phone(i), which)
    seen = riders_on_map(phone(i), others, frames)
    whole = sum(all(s in f for s in others) for f in seen)
    each = ", ".join(f"{NAMES[s]} {sum(s in f for f in seen)}" for s in others)
    print(f"map: phone {i}, OsmAnd {osmand_version(phone(i))}, {'OpenGL' if which == 'opengl' else 'Version 1'} "
          f"engine: all {len(others)} others in {whole} of {frames} frames ({100 * whole // frames} %); "
          f"each, of {frames}: {each}")


# ---- the ride ----------------------------------------------------------------------------

def step(text: str) -> None:
    print(f"ride: {text}", flush=True)


def ride() -> None:
    phones = int((RUN / "phones").read_text())
    boards = phones + 1
    for i in range(phones):
        p = phone(i)
        main_screen(p)
        wait_tap(p, "Find boards")
        wait_tap(p, f"LoRaConvoy-C0FFEE{i:02X}")
        pair(p, i)
        wait_status(p, r"Board: sim")
        step(f"phone {i} paired with board {i} over Bluetooth, encrypted")

    names = NAMES[:boards]
    p0 = phone(0)
    main_screen(p0)
    wait_tap(p0, "Group")
    time.sleep(2)
    wait_tap(p0, "Group name")
    adb(p0, "shell", "input", "text", GROUP.replace(" ", r"\ "))
    wait_tap(p0, "Riders' names")
    adb(p0, "shell", "input", "text", r",\ ".join(names))
    adb(p0, "shell", "input", "keyevent", "KEYCODE_BACK")
    wait_tap(p0, "Create")
    created = None
    for _ in range(10):  # the app writes its preferences in the background
        prefs = adb(p0, "shell", "run-as", APP, "cat", "shared_prefs/group.xml", check=False)
        if m := re.search(r'name="created">(LC2[^<]+)<', prefs):
            created = m.group(1)
            break
        time.sleep(1)
    assert created, "phone 0 kept no group"
    raw = bytearray(base64.urlsafe_b64decode(created[4:] + "=" * (-len(created[4:]) % 4)))

    def invitation(slot: int) -> str:  # as phone 0's Send button shares it
        raw[17] = slot
        return "LC2." + base64.urlsafe_b64encode(bytes(raw)).decode().rstrip("=")

    key = bytes(raw[1:17]).hex()
    step(f"phone 0 started {GROUP!r} for {', '.join(names)}")
    for i in range(1, phones):
        p = phone(i)
        main_screen(p)
        wait_tap(p, "Group")
        time.sleep(2)
        wait_tap(p, "Paste an invitation")
        adb(p, "shell", "input", "text", invitation(i))
        adb(p, "shell", "input", "keyevent", "KEYCODE_BACK")
        wait_tap(p, "Join")
        step(f"phone {i} joined as {names[i]}")
    beacon = boards - 1
    assert all(r.startswith("OK group=") for r in board_command(beacon, f"GROUP {key} {beacon} {boards} 2000"))
    step(f"board {beacon} rides alone as {names[beacon]}, keyed by hand")

    for i in range(phones):
        wait_status(phone(i), rf"{names[i]}, rider {i + 1} of {boards}, in {GROUP}.*{boards - 1} riders heard", 90)
    step(f"every phone's board hears the {boards - 1} others")

    p1 = phone(1)
    main_screen(p1)
    wait_tap(p1, "Need help")
    wait_status(p1, rf"calling for help; {boards - 1} riders' boards have answered", 60)
    step(f"phone 1 called for help, and all {boards - 1} other boards answered")
    wait_tap(p1, "Call off help")
    wait_status(p1, r"^(?!.*calling for help).*Board:", 30)

    # OsmAnd stopped and started again, as a rider or an update does: the app binds to it anew. The
    # Version 1 engine, as OpenGL hides moving points in a share of frames (T-osmand-opengl).
    engine(p0, "v1")
    others = list(range(1, boards))
    seen = riders_on_map(p0, others)
    with open(RUN / "phone-0-map.png", "wb") as f:
        f.write(subprocess.run(["adb", "-s", p0, "exec-out", "screencap", "-p"], capture_output=True).stdout)
    # whether each shows at all, in any of ten frames; how often, the blinking, is bench.py map's
    missing = [names[s] for s in others if not any(s in f for f in seen)]
    assert not missing, f"phone 0's map never showed {missing} in {len(seen)} frames"
    step(f"OsmAnd restarted, and phone 0's map shows each of the {len(others)} others again "
         f"({RUN / 'phone-0-map.png'})")

    restart_board(0)
    wait_status(p0, rf"{names[0]}, rider 1 of {boards}.*{boards - 1} riders heard", 90)
    board_says(0, "<- ", 60)  # its commands came, and without a new pairing: the bond was kept
    assert "paired" not in (RUN / "board-0.log").read_text()
    step("board 0's Bluetooth restarted with its bond kept, and phone 0 came back to it without pairing again "
         "(its node core ran on)")

    restart_board(0, forget=True)
    pair(p0, 0, 180)
    wait_status(p0, rf"{names[0]}, rider 1 of {boards}.*{boards - 1} riders heard", 90)
    step("board 0's Bluetooth restarted without its bonds while phone 0 was away; phone 0 paired again once "
         "the rider agreed")

    main_screen(p1)
    wait_tap(p1, "Forget this board")
    wait_status(p1, r"Stopped\.", 30)
    board_says(1, "forgot its phone")
    step("phone 1 forgot its board: both sides dropped the bond")

    # One owner: with phone 0 away (a connected board does not advertise, as the firmware's
    # one-connection NimBLE does not), phone 1, free now, tries board 0, which turns it away.
    main_screen(p0)
    wait_tap(p0, "Stop")
    main_screen(p1)
    wait_tap(p1, "Find boards")
    wait_tap(p1, "LoRaConvoy-C0FFEE00", 60)
    board_says(0, "it has an owner", 60)
    main_screen(p1)
    wait_tap(p1, "Stop")
    main_screen(p0)
    wait_tap(p0, "Find boards")
    wait_tap(p0, "LoRaConvoy-C0FFEE00", 60)
    wait_status(p0, rf"{names[0]}, rider 1 of {boards}.*{boards - 1} riders heard", 90)
    step("board 0 turned phone 1 away, having an owner; phone 0 came back to it without pairing")
    print("ride: passed")


def lock() -> object:
    """Held until this process ends, by any way it ends."""
    CACHE.mkdir(parents=True, exist_ok=True)
    f = open(CACHE / "bench.lock", "a+")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        f.seek(0)
        sys.exit(f"the bench is busy: {f.read().strip() or 'another bench.py'} runs; wait for it, or stop it")
    f.truncate(0)
    f.write(f"bench.py {' '.join(sys.argv[1:])} (pid {os.getpid()})")
    f.flush()
    return f


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    held = lock() if cmd else None
    if cmd == "up":
        up(int(sys.argv[2]) if len(sys.argv) > 2 else 2)
    elif cmd == "ride":
        try:
            ride()
        except AssertionError:
            # what each phone showed when it failed, for whoever reads the log
            for i in range(int((RUN / "phones").read_text())):
                with open(RUN / f"phone-{i}-failed.png", "wb") as f:
                    f.write(subprocess.run(["adb", "-s", phone(i), "exec-out", "screencap", "-p"],
                                           capture_output=True).stdout)
                (RUN / f"phone-{i}-failed.txt").write_text("\n".join(t for t, _, _ in screen(phone(i))))
            print(f"ride: FAILED; each phone's screen at the failure is in {RUN}/phone-*-failed.*")
            raise
    elif cmd == "down":
        down()
    elif cmd == "engine" and len(sys.argv) == 4 and sys.argv[3] in ("opengl", "v1"):
        engine(phone(int(sys.argv[2])), sys.argv[3])
    elif cmd == "map" and len(sys.argv) >= 4 and sys.argv[3] in ("opengl", "v1"):
        measure_map(int(sys.argv[2]), sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 30)
    elif cmd == "ui" and len(sys.argv) >= 4:
        serial = phone(int(sys.argv[2]))
        if sys.argv[3] == "list":
            for t, x, y in screen(serial):
                print(f"{x:5d} {y:5d} {t!r}")
        elif sys.argv[3] == "tap" and len(sys.argv) > 4:
            print("tapped" if tap(serial, sys.argv[4]) else "not on screen")
    else:
        sys.exit(__doc__)
