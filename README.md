# lora-convoy

Research into sharing positions within a group ride over LoRa, with no mobile network and no
infrastructure: what existing systems do, where they fall short, and what a protocol designed for
a moving group could do better.

**Status:** research notes and an airtime calculator. No firmware or app yet, and nothing here has
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
| 07 | [Experiments](docs/07-experiments.md) | What to measure, and the log |

`tools/airtime.py` produces every time-on-air, duty-cycle and capacity figure in these documents,
and `tools/route_size.py` the route sizes (GPX in, simplified binary sizes and LoRa packets out):
`python3 tools/airtime.py` prints the tables, `python3 tools/airtime.py 56 7 250` one packet.

## Conventions

Each claim says where it comes from:

- **[code]** — read in the project's source, main branch on the date given;
- **[docs]** — the project's own documentation or a vendor page;
- **[calc]** — computed with `tools/airtime.py`;
- **[bench]** — measured on our boards;
- **[owner]** — the project owner's experience on the road with the apps concerned.

Anything untagged is inference. Regulatory notes are information, not legal advice.
