# 07. Experiments

## Boards on hand (2026-10-17)

| Board | Radio | Now runs | Notes |
|---|---|---|---|
| Seeed Wio Tracker L1 Pro | nRF52840 + SX1262, GPS (L76K), OLED | Meshtastic 2.7.26.54e0d8d, owner `morlov` / `mrlv`, EU_868, LongFast, Client, public channel | paired with the owner's phone |
| LILYGO T-Deck | ESP32-S3 + SX1262 | stock Meshtastic 2.7.10 with its settings from 2026-09-21 | |
| Heltec WiFi LoRa 32 V4.3 | ESP32-S3 + SX1262 + amplifier | dmd2-lora-bridge bench build | can take Meshtastic |
| ESP32 + EBYTE E220, ESP32 + EBYTE E22 | modules | dmd2-lora-bridge bench builds | on the remote bench station |
| Nooelec RTL-SDR | receiver | — | for reading the air ([dmd2-lora-bridge](https://github.com/MiklerGM/dmd2-lora-bridge) `bench/sdr_lora.py`) |

## Planned

| # | What | Answers |
|---|---|---|
| E1 | L1 and T-Deck on a private channel, MediumFast, smart position every 20 s; a second run with both as Client Mute (no relays) | real intervals, relay delay, losses; how much "no relays" gains |
| E2 | Range, ShortFast against MediumFast, one board in a vehicle (the Range Test module is in the L1's build) | the range table in [02](02-radio-basics.md) |
| E3 | A minimal app that draws two points in OsmAnd over AIDL and moves them every second | whether the `MAP` group is granted without asking, smoothness |
| E4 | How many BLE clients a Meshtastic node accepts; whether a USB client and a BLE client work at once | whether a companion must replace the official app |
| E5 | MeshAnd with the L1 and T-Deck | whether it is a usable baseline; what is missing |

## Log

- **2026-10-15** [bench]: the dmd2-lora-bridge firmware on the L1 sent a group position every ~2 s,
  each 314 ms on air: the reference for [04](04-dmd-protocol.md).
- **2026-10-17** [calc]: route sizes from two real GPX files (a 4 920 km multi-day route, the
  3 157 km Trans-Morocco Trail map): 4–7 MB of GPX become 21–65 kB of binary deltas at 5–50 m
  tolerance, and 0.25–0.8 kB per 200 km at 100–500 m for road snapping ([06](06-own-protocol.md)).
- **2026-10-16:**
  - The L1 went back to stock and was updated to Meshtastic 2.7.26 from the official release. The
    over-the-air update from the phone app had stopped in the bootloader, so it was copied over USB.
  - Configured with owner `morlov` / `mrlv`, region EU_868, Client, LongFast.
  - The T-Deck went back to stock Meshtastic 2.7.10 from a full dump taken before the bench.
- **2026-10-16** [bench]: after a region change, Meshtastic 2.7.15 on the L1 stopped answering over
  USB and Bluetooth, with a dark screen. After a full power-off the screen came back, and 2.7.26
  behaves normally.

  The dark screen was dmd2-lora-bridge's doing: its OLED off routine also turns off the SH1106's
  charge pump, which the next firmware does not turn back on until power is removed.
