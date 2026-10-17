# lora-convoy

Research into sharing positions within a group ride over LoRa, with no mobile network and no
infrastructure: what existing systems do, where they fall short, and what a protocol designed for
a moving group could do better.

**Status:** research notes and calculation tools. No firmware or app yet, and nothing here has
been ridden.

## Why this exists

[dmd2-lora-bridge](https://github.com/MiklerGM/dmd2-lora-bridge) put stock DMD2 on a phone into
DMD's LoRa group rides, with our own app and radio boards. It worked on rides, and it showed how
far DMD's protocol is from what a group needs (see [04](docs/04-dmd-protocol.md)). From DMD2 4.036
on, DMD2 talks only to DMD's own LoRa app, so that route is closed. This repository looks at the
alternatives from scratch: off-the-shelf mesh firmware, existing products, and a purpose-built
protocol, with OsmAnd as the map.

## Reading order

| | Document | What it answers |
|---|---|---|
| 01 | [Goals and requirements](docs/01-goals.md) | What a group ride needs; open questions |
| 02 | [Radio basics](docs/02-radio-basics.md) | SF, bandwidth, time on air, duty cycle, channel load, capacity |
| 03 | [**Alternatives**](docs/03-alternatives.md) | Meshtastic, MeshCore, Reticulum, products; comparison |
| 04 | [The DMD protocol](docs/04-dmd-protocol.md) | What it does well and where it breaks |
| 05 | [OsmAnd integration](docs/05-osmand-integration.md) | Getting other riders onto the OsmAnd map |
| 06 | [**Our own protocol**](docs/06-own-protocol.md) | What a purpose-built design could do better, and at what cost |
| 07 | [Experiments](docs/07-experiments.md) | What to measure, and the results |

## Tools

All standard-library Python:

| Tool | What it does | How to run |
|---|---|---|
| `tools/airtime.py` | every time-on-air, duty-cycle and capacity figure in the documents | `python3 tools/airtime.py` prints the tables; `python3 tools/airtime.py 56 7 250` prices one packet |
| `tools/route_size.py` | route sizes: a GPX in, simplified binary sizes and LoRa packets out | `python3 -I tools/route_size.py route.gpx --tolerances 2,5,100` |
| `tools/cross_check.py` | re-derives airtime independently, from Semtech's driver, and checks the other two tools against it | `python3 -I tools/cross_check.py` |

## Conventions

Each claim says where it comes from:

- **[code]** — read in a project's source, pinned to a commit or release tag;
- **[docs]** — the project's own documentation, a vendor page or a regulation;
- **[calc]** — computed with the tools above;
- **[bench]** — measured on our boards;
- **[owner]** — the project owner's experience on the road with the apps concerned.

Anything untagged is inference. Regulatory notes are information, not legal advice.

Meshtastic's firmware branch `develop` (pinned here at `fad52bb`) is ahead of the release on our
boards, 2.7.26 (tag `54e0d8d`): several behaviours below exist only in `develop`, and each says
which.

These documents were reviewed by Codex on 2026-10-17. Its findings have been folded into the text;
its original annotations are in the repository history, commits `62f3fca` to `75ae2c1`.

Review (Codex qwen, 2026-10-17): I re-derived every time-on-air and capacity figure in
these documents from the Semtech formula with exact rationals, independently of `tools/airtime.py`
and `tools/cross_check.py`. All of them reproduce: 56 B SF7/250 pre16 = 57.984 ms, SF9 =
190.976 ms; 24 B pre8 = 30.848 / 56.576 / 102.912 ms at SF7/SF8/SF9/250 and 370.688 ms at
SF10/125; 246 B = 192.128 ms at SF7/250, 604.672 ms at SF9/250, 2213.888 ms at SF10/125; MeshCore
37 B SF8/62.5 CR 4/8 with a 32-symbol preamble = 508.928 ms; the slot caps 19.58 / 12.02 / 7.09
per second and the 20-rider frames 1.021 / 1.664 / 2.823 s; the SF10/125 route fit of 110.8 min.
The arithmetic in [02](docs/02-radio-basics.md) and [06](docs/06-own-protocol.md) can be
trusted. [calc]

Every project pinned in [03](docs/03-alternatives.md) is mirrored locally at `../upstream/`, at
the commits in its pin table (meshtastic-firmware `fad52bb`, mt-fw-2726 `54e0d8d`,
meshtastic-android `ed0a53f`, meshtastic-atak-plugin `4c4766c`, meshcore `81dbae5`,
meshcore-solo `1f47fc5`, reticulum `e40191b`, sideband `ef572b5`, meshand `034e24d`,
sx126x-driver `a10c5df`, osmand `a68b83f`), so any `[code]` claim here can be checked offline.
