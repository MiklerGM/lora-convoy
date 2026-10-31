# DES research simulator

The event engine and models use Python's standard library. `firmware/sim/bridge.c` builds a
small host library around the **actual C node and codec**, not a Python implementation of v3.
The original stepped simulator and `tools/compare.py` remain available as historical baselines.

From the repository root:

```sh
python3 tools/simulate.py --scenario bridge-handover --seeds 17,29,43 --output /tmp/convoy-bridge-des
python3 tools/simulate.py --suite --shuffle-slots --action-phase-s 2 --output /tmp/convoy-des-suite
python3 tools/simulate.py --generate neighbours --size 20 --seeds 17 --output /tmp/convoy-two-groups
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

## What actually runs

| Candidate | Implementation and limitations |
|---|---|
| `v3` | The production node/codec with its existing help acknowledgements; explicit header and CRC only |
| `relay` | Research C adapter using the same node/codec, adding a four-byte `SR`/version/count envelope and up to eight intact foreign v3 packets |

`relay` forwards the locally cached sources least recently forwarded, expires cache entries by
time since their **new authenticated packet** was accepted, preserves the original source packet,
and charges the complete RF frame against its node's native ledger. It uses the configured
maximum frame length for time guards, and can reduce forwarded records to fit remaining budget.
Duplicates do not renew the cache. The configured TTL is a local-cache policy, not a wire-carried
source-transmission timestamp. It is neither sdk firmware nor a selected v4 format.

Source packets retain their original authentication. The research envelope itself is not
authenticated and does not communicate a relay-peer identity. The observer knows the physical
RF sender; that knowledge is not given to the candidate. This adapter inherits v3's two-state
help behaviour and cannot transmit before its first position fix. It does not implement the
agreed future five-state interface or establish a security claim for a relay protocol.

The Python adapter passes only local time, GNSS/PPS, commands and radio observations into C.
Evaluator provenance is derived from actual source packet identities and retained separately.
Record authors, RF senders, groups and slots are distinct in observations and metrics.

## Models and input

JSON schema version 1 is validated by `world.py`. Six checked-in examples are in
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
  conversion currently uses a local approximation anchored at 47.1410 N, 9.5215 E.
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

## Evaluation and evidence

True ages are integrated by elapsed time rather than sampled at a fixed phase. Report coverage
over every within-group pair separately from conditional age percentiles. Displayed ages and
clock errors remain separate; the displayed calendar is piecewise affine between observations,
with the node's tick-resolution error bound reported. Unknown provenance is not repaired by
choosing a convenient truth record. A reboot clears the receiver view and cannot retain a
positive fork-information result from its previous boot.

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
