# DES research simulator

The event engine and models use Python's standard library. `firmware/sim/bridge.c` builds a
small host library around the **actual C node and codec**, not a Python implementation of v3.
The original stepped simulator and `tools/compare.py` remain available as historical baselines.

From the repository root:

```sh
python3 tools/simulate.py --scenario bridge-handover --seeds 17,29,43 --output /tmp/convoy-bridge-des
python3 tools/simulate.py --suite --shuffle-slots --action-phase-s 2 --output /tmp/convoy-des-suite
python3 tools/simulate.py --generate neighbours --size 20 --seeds 17 --output /tmp/convoy-two-groups
python3 tools/simulate.py --scenario hidden-terminals --candidates v3,v3-cad,relay,relay-cad --assumptions tools/simulation/assumptions/radio.json --output /tmp/convoy-sensitivity
python3 tools/simulate.py --scenario chain-5 --setups tools/simulation/setups/phy-cadence.json --output /tmp/convoy-phy-cadence
make -C firmware simulator ARGS="--generate chain --size 10 --seeds 17 --output /tmp/convoy-chain"
make -C firmware test-simulator
```

Output directories must be new or empty. Each run retains the normalized scenario, event log,
summary and an offline HTML replay. Open `replay.html` directly; it needs no server or external
assets. `--no-replay` omits it. `--reference` additionally computes a causal, unlimited-content
diagnostic on the actual receive trace; its payload is free, so it is not a realizable protocol.

`--candidates v3,relay` is the default. `--interval-ms` overrides the configured cadence explicitly.
The generators are `dense`, `chain`, `bridge` and `neighbours`, with 2–32 participants per group.
Independent groups reuse slot numbers without reusing identity; a world can contain more than
32 boards. Each seed produces separate environment, layout and private protocol randomness.
Group keys vary by layout seed; slot permutation and action phase variation are explicit options.
`--assumptions` evaluates explicitly named radio/platform cases against the same exogenous
worlds and private candidate seeds. Its JSON schema is `{"schema":1,"cases":[{"name":"label",
"radio":{...}}]}`. Receiver models, thresholds, timing or CAD errors may vary; a seed override
is rejected. Reports retain per-world differences and mark sign reversals across the selected
assumptions. No overall winner or statistical confidence is inferred from a few worlds.
The checked-in cases are synthetic hypotheses, not measured capture thresholds.

`--setups` names candidate configurations separately from their implementation. Each entry in
`{"schema":1,"candidates":{"label":{"implementation":"relay","settings":{...}}}}` can override
`interval_ms`, `phy`, `duty_permille`, `listen_one_in`, `relay_records`, `relay_ttl_s`,
`tx_power_dbm`, and the send rule's `beat` (a heartbeat every 2^beat frames) and `move_m`; sensor
truth, membership and trajectories cannot be changed there. A node left alone sends every frame
(beat 0) and carries two riders; a board sends a heartbeat in eight frames (beat 3), on a move
of 10 m, every 2 s, and carries one. `setups/board.json` names that board as `board`, for runs
meant to say what a ride on the boards would see. PHY settings
merge with each node's declared mode. With no `--candidates`, the file's entries are selected;
otherwise that flag selects labels explicitly. `--interval-ms` is the final cadence override.

Use the same settings for comparisons of access/forwarding at a common offered workload. To
compare each candidate's chosen feasible profile, supply different settings and assess the
different offered loads explicitly. The example `phy-cadence.json` only illustrates this second
experiment; it does not select field profiles or establish range. Results retain labels, actual
implementations, requested settings, CLI cadence override and normalized effective node setups.
The exogenous hash excludes protocol settings and normalized MODE PHY values. MODE timing and
targets remain paired; its effective mode also depends on the candidate's starting PHY and is
retained in the complete scenario hash. World equality alone does not establish identical radio
configurations.

## What actually runs

| Candidate | Implementation and limitations |
|---|---|
| `v3` | The production node/codec, since 2026-11-14 protocol v5, carrying no other riders; explicit header and CRC only. The name is kept for the setups that use it |
| `relay` | The same core carrying `relay_records` other riders in each packet (v5 passengers, 0–3); `relay_ttl_s` is no longer used, the core forgets a rider after 10 minutes |
| `v3-cad`, `relay-cad` | The corresponding payload/forwarding logic with a research C policy that senses before its own scheduled transmission |

Both run the board's own send rule and passenger choice (`node.c`); a carried record keeps the
second its rider said it, so it never looks younger, and the host keys records by that second,
place and state, which every teller of a rider agrees on.

Carried records travel inside their carrier's authenticated packet. The observer knows the
physical RF sender; the core knows only the slot that carried a record. The host drives two states
(normal, help) of the five the core has.

The CAD policies prepare their real packet in the nominal slot, then use only local CAD results.
They make at most three attempts, with a private seeded 1–20 ms backoff after a busy channel or
refused CAD request. CAD and preparation consume the remaining symmetric timing guard; an
expired or MODE-cancelled packet is dropped. A refused request is not a successful scan.
Booked-but-dropped native cost remains separate from actual RF cost. These are experiments in
sensing before one's own slot, not unslotted CSMA, Meshtastic, or evidence of regulatory LBT.

The Python adapter passes only local time, GNSS/PPS, commands and radio observations into C.
Evaluator provenance is derived from actual source packet identities and retained separately.
Record authors, RF senders, groups and slots are distinct in observations and metrics.

## Models and input

JSON schema version 1 is validated by `world.py`. Nine checked-in examples are in
[`tools/simulation`](../simulation). Inputs reject unknown properties and duplicate JSON keys.
Identifiers use letters, digits, underscore, dot and dash; `all` is reserved as an outage target.

```json
{
  "schema": 1,
  "name": "two-participants",
  "duration_s": 60,
  "warmup_s": 5,
  "default_link": {"decode": true, "sense": true, "interfere": true, "power_dbm": -90},
  "nodes": [
    {"id": "a", "group": "ride", "slot": 0,
     "trajectory": [{"t_s": 0, "x_m": 0, "y_m": 0}, {"t_s": 60, "x_m": 600, "y_m": 0}]},
    {"id": "b", "group": "ride", "slot": 1,
     "trajectory": [{"t_s": 0, "x_m": 100, "y_m": 0}, {"t_s": 60, "x_m": 700, "y_m": 0}]}
  ],
  "noise": {"background_dbm": -120,
            "bursts": [{"from_s": 20, "until_s": 21, "receivers": ["b"], "power_dbm": -70}]},
  "radio": {"noise_threshold_dbm": -100},
  "actions": [{"id": "help", "t_s": 30, "node": "a", "kind": "state", "state": "help"}]
}
```

The numbers above are synthetic inputs, not measurements or prescribed radio settings.

- Trajectories are piecewise-linear metric XYZ keyframes. GNSS has separate measured coordinates,
  UTC, validity, production and delivery time, noise and serial jitter. The native geographic
  conversion uses a declared local approximation at `origin: {lat_deg, lon_deg}` (default
  47.1410 N, 9.5215 E). It is not a geodesic or long-distance projection.
- PPS is produced independently at whole UTC seconds. It precedes same-time NMEA processing;
  changing GNSS measurement cadence cannot change PPS frequency.
- Clocks have offset, resolution and drift, optionally a piecewise `profile` of
  `{from_s, drift_ppm}`. Local deadlines invert the integrated physical clock. Reboot resets
  the counter while the absolute drift/temperature profile continues.
- Directed `links` can have `from_s`/`until_s` and separate decoding, sensing and interference
  permissions. With no declared power, the symbolic model does not invent measured RSSI.
- Rectangular endpoint `zones` schedule all trajectory boundary crossings. A fractional-nanosecond
  crossing includes the neighbouring representable instants, preserving half-open areas.
- `propagation` supports a declared reference-power/log-distance model with a required update
  interval. `fading` adds a seeded, correlated, piecewise-constant AR(1) power field, scoped to
  a line, receiver or the world. Its grid and correlation parameters are explicit synthetic inputs.
- External noise bursts can affect selected receivers. Multiple active powers are summed linearly.
  Reactive neighbouring groups execute real node instances and are not substituted by noise.
- `noise.processes` adds reproducible alternating exponential OFF/ON sources. Each entry specifies
  `id`, `from_s`, `until_s`, `mean_on_s`, `mean_off_s`, `power_dbm` and optional `receivers`.
  It starts OFF; omitted receivers mean one shared global process. All transitions are generated
  from the environment seed and process identity before any candidate runs, with waits rounded
  up to nanoseconds and clipped at the window end. Other traffic and query order cannot redraw
  it. The initial OFF condition is explicit; short windows need not have the stationary ON fraction
  `mean_on_s / (mean_on_s + mean_off_s)`. Zero means, duplicate IDs and excessive transition budgets
  are rejected.
- Reception models are conservative erasure, threshold capture and integrated-energy hypotheses.
  Power models require an explicit threshold and known powers. These are not calibrated LoRaSim,
  ns-3 or SX1262 models. With the conservative model, power variation affects reception only if
  a sensitivity/noise threshold is explicitly supplied.
- Radio state includes acquisition, header, payload, CAD, transmission, switching and sleep.
  Full-packet histories account for internal interference intervals. RF settings are snapshots
  of the confirmed applied mode. MODE, abort, reboot and delayed IRQ generations are modelled.

Radio timing defaults, configured thresholds and supported/unsupported features are emitted in
every summary. Failed decoding is an erasure, including no-CRC modes; corrupted-byte delivery
and receiver recapture are explicitly unsupported. CAD is a local model with configurable errors;
its existence does not establish that a candidate implements legal or useful LBT.
`irq_delay_ns` delays observation delivery; the supplied event stamp still refers to the ideal
completion edge and is then quantized by the node clock. ISR latency bias in that stamp is not
modelled. A 31 microsecond clock tick therefore still does not establish a real L1 timestamp's
accuracy; measured E0 latency is needed to extend and calibrate that part.

## Evaluation and evidence

True ages are integrated by elapsed time rather than sampled at a fixed phase. Report coverage
over every within-group pair separately from conditional age percentiles. Displayed ages and
clock errors remain separate; the displayed calendar is piecewise affine between observations,
with the node's tick-resolution error bound reported. Unknown provenance is not repaired by
choosing a convenient truth record. A reboot clears the receiver view and cannot retain a
positive fork-information result from its previous boot.

Position error uses the actual observed XY coordinates, including sensor and codec error. With
piecewise-linear truth, its squared-distance integral is exact across trajectory corners and view
changes; reports include conditional mean squared error, RMS, maximum and unavailable duration.
Missing positions are not counted as zero error. This metric neither assumes known provenance
for AoI nor invents altitude that the wire format does not carry. Position-error quantiles are
not computed.

Radio, new-source-record and new-fix gaps are distinct and retain censored ends. State correctness
does not itself prove receipt of a particular cancellation. Per-node RF cost uses actual occupied
intervals, including aborts and a censored run end; rolling-hour checks are independent of the
native minute-bucket ledger. Runtime checks executed and not executed are listed explicitly.

The checked-in fork case measures receipt of a source fix after a declared decision threshold;
it does not model a human seeing and interpreting the map. Android/Bluetooth/OsmAnd are not in
this execution. The existing Android bench remains a separate evidence level.

Current unimplemented parts of the full [design](../../docs/11-simulation-model.md) include DEM
propagation, measured calibration datasets, actual Meshtastic/SDK adapters, corrupted payloads,
receiver recapture, energy use and real phone delivery. The simulator and research relay are
not field-validated. The implementation status is intentionally distinct from the design target.
