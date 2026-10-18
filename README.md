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

## Reviews

Two AI reviews, each re-deriving the figures and checking claims against pinned sources, have been
folded into the text. Their original annotations are in the repository history:

- Codex, 2026-10-17: commits `62f3fca` to `75ae2c1`;
- Qwen, 2026-10-17: commits `4009774` to `edef682`. Three further agents (Fable, Opus, Sonnet)
  judged its findings independently; where they disagreed with it, the text follows the evidence
  they found.

Every `[code]` claim is checkable offline against local clones of the projects pinned in
[03](docs/03-alternatives.md).
