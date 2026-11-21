# lora-convoy

Research into sharing positions within a group ride over LoRa, with no mobile network and no
infrastructure: what existing systems do, where they fall short, and what a protocol designed for
a moving group could do better.

**Status:** research notes and calculation tools, and the start of building it: architecture,
roadmap and backlog below; an app that shows riders and routes in OsmAnd and sets up a private
group, and firmware for the Heltec V4.3 and the Wio Tracker L1 Pro that sends positions encrypted
for the group, each rider in a slot by GPS time, answers calls for help, and talks to the app over
Bluetooth, all tested without hardware (M1–M6, and M7 in progress). Two boards have run on a desk, where the schedule held ([07](docs/07-experiments.md)); nothing here has been ridden.

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
| 08 | [Regions](docs/08-regions.md) | Which bands, power and access rules apply in which country |
| 09 | [The protocol's next version](docs/09-protocol-next.md) | What an outside review of v3 found, and the order to settle it in |
| 10 | [Capabilities and protocol comparison](docs/10-protocol-comparison.md) | Wanted behaviour, common scenarios, measurements and the runnable draft baseline |
| 11 | [Simulation model](docs/11-simulation-model.md) | The target DES, world/radio models, candidate boundaries, validation and calibration |
| 12 | [Proposal: protocol v5](docs/12-protocol-v5-proposal.md) | A review of v3 and of v4's plan; profiles by activity, sending by movement, one packet, a shared plan, and one test rig. Not decided |
| 13 | [Test plan: two boards and an SDR](docs/13-test-plan.md) | What to measure with a Heltec, an L1 and an RTL-SDR, in what order, and what each stage decides. Stages 0–3 run |

## Building it

| Document | What it answers |
|---|---|
| [Architecture](docs/ARCHITECTURE.md) | How board, app and OsmAnd split the work; the decisions behind it |
| [Roadmap](docs/ROADMAP.md) | Milestones in order, each with a test without hardware and one on it |
| [Backlog](docs/BACKLOG.md) | Open decisions and features not yet in a milestone, one ticket each |
| [Protocol](docs/PROTOCOL.md) | The air format, radio mode, invitation and phone API, as built: v5 |
| [Bench](docs/BENCH.md) | What to flash and check, step by step, once the boards are on the bench |

Nothing is stable yet: the air format, the phone API and the app may all change together.

The app is in `android/`, the firmware in `firmware/`. `make check` runs every test that needs
no hardware and builds both board images; `android/emulator.sh` sets up an emulator with OsmAnd,
and `make -C firmware sim ARGS="10 2000 600"` simulates ten riders for ten minutes.
`tools/bench/bench.py up`, `ride` and `down` run a whole group on one computer: emulated phones
with the app and OsmAnd, each paired over virtual Bluetooth with its own simulated board, the
boards on simulated air ([tools/bench](tools/bench/README.md)).

## Tools

All standard-library Python, but for the bench's Bluetooth boards (Bumble, in a venv):

| Tool | What it does | How to run |
|---|---|---|
| `tools/airtime.py` | every time-on-air, duty-cycle and capacity figure in the documents | `python3 tools/airtime.py` prints the tables; `python3 tools/airtime.py 56 7 250` prices one packet |
| `tools/route_size.py` | route sizes: a GPX in, simplified binary sizes and LoRa packets out | `python3 -I tools/route_size.py route.gpx --tolerances 2,5,100` |
| `tools/cross_check.py` | re-derives airtime independently, from Semtech's driver, and checks the other two tools against it | `python3 -I tools/cross_check.py` |
| `tools/compare.py` | runs controlled topologies and timing/loss environments; reports missing data, freshness and air use | `python3 tools/compare.py --output /tmp/convoy-comparison`; `--suite sensitivity` varies the environment |
| `tools/simulate.py` | DES worlds, real C v3/research relay/CAD policies, exact time-weighted metrics, model sensitivity and offline replay | `python3 tools/simulate.py --scenario chain-5 --output /tmp/convoy-des`; [models and limits](tools/simulator/README.md) |

## Conventions

Each claim says where it comes from:

- **[code]** — read in a project's source, pinned to a commit or release tag;
- **[docs]** — the project's own documentation, a vendor page or a regulation;
- **[calc]** — computed with the tools above;
- **[bench]** — measured on our boards, or on an emulator where it says so;
- **[owner]** — the project owner's experience on the road with the apps concerned.

Anything untagged is inference. Regulatory notes are information, not legal advice.

Meshtastic's firmware branch `develop` (pinned here at `fad52bb`) is ahead of the release on our
boards, 2.7.26 (tag `54e0d8d`): several behaviours below exist only in `develop`, and each says
which.

## Licence

GPL-3.0-or-later ([LICENSE](LICENSE)), for the firmware, the app, the tools and the documents
([ARCHITECTURE](docs/ARCHITECTURE.md), D14).

## Reviews

Two AI reviews, each re-deriving the figures and checking claims against pinned sources, have been
folded into the text. Their original annotations are in the repository history:

- Codex, 2026-10-17: commits `62f3fca` to `75ae2c1`;
- Qwen, 2026-10-17: commits `4009774` to `edef682`. Three further agents (Fable, Opus, Sonnet)
  judged its findings independently; where they disagreed with it, the text follows the evidence
  they found.

Every `[code]` claim is checkable offline against local clones of the projects pinned in
[03](docs/03-alternatives.md).
