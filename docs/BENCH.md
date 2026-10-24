# Bench

What to do once the boards are on the bench, in order. Each step closes a milestone's "Test on
hardware" in [ROADMAP](ROADMAP.md); until then they are "done, no hardware". Console logs and
captures go in `lab/`, which is not committed.

## Flashing

| Board | Build | Flash | Console |
|---|---|---|---|
| Heltec V4.3 with its GNSS kit | `make -C firmware esp` | `cd firmware && pio run -e heltec_v4 -t upload` | `pio device monitor -e heltec_v4`, 115200 |
| Wio Tracker L1 Pro | `make -C firmware nrf` | double-tap reset, or `BOOTLOADER` on the console; copy `firmware/zephyr/build/zephyr/zephyr.uf2` to the `TRACKER L1` drive | the board's USB serial port (Zephyr CDC ACM) |

Both boards answer the phone API's commands typed on the console ([PROTOCOL](PROTOCOL.md)).

## 1. One board alone (M3)

- The start line: `LoRa Convoy, protocol v3, phone API 5, on heltec_v4 after poweron: no key yet …,
  slot N of 32, every 2000 ms, 14 dBm`.
- Outdoors, within a few minutes: `GNSS fix=1 time=1 sats=…`. If the Heltec's GNSS stays silent,
  VEXT (GPIO 36, active low) may need to be on as well (firmware/src/main.c).
- `STATUS`: `time=pps` on the Heltec, `time=nmea` on the L1; `err=` and `budget=` in ms.

## 2. E0: each board's time

- **Heltec:** after a few minutes, `lag=last/min/max` in `STATUS` is how far the L76K's NMEA fix
  lags behind its PPS edge. Note all three. If the most nears a second, the pairing is wrong (T-timing).
- **L1:** it has no PPS, so it takes the Heltec's numbers: build it with
  `-DNODE_NMEA_LAG_US=<the lag>` and `-DNODE_NMEA_ERR_US=<half the spread, plus a margin>`.
  Without them it assumes 0 ± 100 ms, and keeps quiet in groups of more than seven.
- **Both:** `late=` and `unsure=` stay at 0 with a fix. `late=` growing means the radio task wakes
  too late for its slot.

## 3. Two boards in one group (M5, M6)

On the consoles, or later from the app:

```
GROUP 000102030405060708090a0b0c0d0e0f 0 2 2000     on one: the test vector's key; group=86c91c92
GROUP 000102030405060708090a0b0c0d0e0f 1 2 2000     on the other
```

- `TX counter=… ok` once every 2 s on each, and a `RIDER slot=…` line on the other for each one.
- `STATUS`: `riders=1`, `clash=0`, and `rx=` in the RIDER line near its counter run: the delivered
  share.
- A third board with another key: `foreign=` grows on the two, and it never shows as a rider.

## E2: range

With two boards in one group, `MODE 9 250` on both (or another of the modes in
[06](06-own-protocol.md), Sizing) trades cadence for range without reflashing; `STATUS` shows
`mode=`, and `budget=` shrinks with the longer packet. Walk or ride one board away and note where
the RIDER lines stop, and their `rssi=` and `snr=` before that. `MODE 7 250` goes back.

## 4. The app (M4, M5)

- Find boards, tap one, accept Android's pairing dialog: the main screen shows `Board: …`.
- Group, Create with the riders' names, then send each other rider their invitation; on each
  other phone, Join with it. Each board's `group=` is the same.
- OsmAnd, Plugins, LoRa Convoy on: the other riders on the map, ageing grey when not heard.
- Forget this board, or the board's button held 3 s: the phone can pair again.

## 5. Help (M7)

- Need help on one phone: the others' maps show that rider red with SOS, and the details say who
  has answered; the caller's screen counts the answers.
- `LEDGER` on a console: transmit time a minute at a time over the last hour.

## What to bring back

The console logs, a `STATUS` from each board after an hour, `LEDGER`, and the three `lag=` numbers
from each Heltec.
