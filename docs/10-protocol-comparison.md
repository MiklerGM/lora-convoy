# 10. Capabilities and protocol comparison

**2026-10-31.** A register of the behaviour we want and a repeatable comparison of the candidates.
The current protocol is a draft with host and emulator checks, not a field-proven design. This
document neither fixes v4's bytes nor chooses a radio profile. [01](01-goals.md) holds the goals,
[09](09-protocol-next.md) the next-format decisions, and this document the comparison contract.
The full simulator target, DES/channel contracts, validation and calibration stages are in
[11](11-simulation-model.md). Its planned capabilities must not be confused with the runnable
baseline described here.
The newer DES implementation is available through [`tools/simulate.py`](../tools/simulate.py).
It runs v3, an experimental C forwarding adapter and local-CAD variants, with paired
radio-model assumptions and exact time-weighted age/position metrics. Its [implementation notes](../tools/simulator/README.md)
separate implemented models from the remaining full-design work. The commands and sampled metrics
below continue to describe the original baseline; do not pool their numbers with DES results.

## Capability register

An **agreed target** describes wanted behaviour, not completed implementation. A **proposed
target** is included for comparison without deciding the first field release's scope. **Open**
means a product choice is still needed. A host result alone never marks hardware support proven.

| Capability | Decision | Current v3 | What establishes it |
|---|---|---|---|
| Positions for a leader and a rider catching up | Agreed target; neither role takes priority | The app puts received riders on OsmAnd; checked on emulators | Observe the required riders on the map, including a rider carrying only a board |
| Honest ages and last observations | Agreed target | Fix age and radio age exist; some displayed movement wording is still pending | An old fix in a new packet stays old; phone delivery delay is counted; a past stop is said to be past |
| A participant carries a ready board | Proposed target, consistent with autonomous boards | A configured board works without its phone; the organiser's flow for configuring several boards is pending | Set up a loaned board, disconnect its setup phone, and keep reporting without an account or action by its carrier |
| Setup follows the roster | Agreed target; ten riders are the first reference, not a maximum | Invitations carry the roster and interval, but the app uses one mode and 2 s for every group | Every board takes the same computed setup; a setup that does not fit its packet and time error is refused |
| One manual state and independent observed facts | Agreed target for v4 | Only help exists, with v3's automatic answers | Carry the agreed five states; cancellation is not undone by an old copy; a restarted board starts in normal state |
| Useful exchange across a stretched group | Proposed target for the described use; first field-release scope is open | Direct reception only | Deliver latest records through a bridge and a chain when a usable path exists; with no path, retain honest last-known data |
| Loss and return of GNSS | Agreed target | Last fix is retained; the slot gate uses an assumed error bound | A lost fix does not refresh the held position; transmission follows the chosen access rules; TDMA stops when time is too uncertain |
| Bounded use of the air | Agreed target | The slot and duty ledger are in the core | Use the full serialized packet in timing and budget checks; include relays and control traffic in each node's cost |
| Heltec with GNSS and Wio Tracker L1 Pro | Target platforms | Both images build; their physical operation is unverified | Check the actual drivers, PPS/NMEA timing, BLE, restarts and power on each platform |

The five manual states are normal, need help, please stop, need fuel and helping. Observed facts
may coexist: a current position, stopped, battery low. Their measurement and thresholds need their
own evidence. No special automatic help answers, addressed helping, separate help button or
persisted manual state are required; those decisions stay in [ARCHITECTURE](ARCHITECTURE.md).

| Open choice | How the comparison treats it |
|---|---|
| Private positions, public reading, or fully open messages | Record confidentiality and authentication separately for each candidate. An unprotected mode does not promise resistance to forged messages; it still needs honest handling of ordinary old copies and duplicates. The existing private mode is not silently removed |
| Relaying in the first field release | Measure it now; keep release scope separate from the research result |
| Battery endurance and external power | Measure the devices under the selected workload. No 20-hour target or endurance result is assumed |
| Plain Meshtastic interoperability | Record whether a candidate interoperates; do not assume it is mandatory |

Displays and a state selector on the board, prediction, chat, route transfer, server sync and iOS
are deferred from this first comparison. They remain possible product work, not rejected features.
An animation or prediction must never change a measured fix's time or observed facts.

## Candidates and evidence

| Candidate | Available evidence | Runnable here |
|---|---|---|
| Current v3 | The production C core and codec; existing host and emulator checks | Yes, as the first baseline; no relaying |
| A design inspired by sdk's description | The reported design and individual link observations; no complete source or eight-node result | Not yet. A reconstruction must be labelled as such, not as a run of sdk's firmware |
| Patched Meshtastic | Proposals against pinned sources in the earlier alternatives review | Not yet. Recheck the chosen revision, implement and identify the exact patch before reporting results |
| A later version of our draft | Design hypotheses in [06](06-own-protocol.md) and [09](09-protocol-next.md) | Only when its real codec and node logic run; a proposed capacity is not a result |

Slot access, carrier sensing, record selection, record count, protection and PHY are independent
choices. First compare only a small set of candidates. Adding a neighbour map or a new radio
mode is justified by a measured benefit, not by the existence of an option.

### From a feature to an experiment

For each proposed change, name the wanted behaviour, the hypothesis, the baseline and the
observation that would support or reject it. Keep implementation choices apart from product
requirements: three forwarded records, a neighbour bitmap or TDMA is not itself a user feature.

| Hypothesis | Discriminating case | Observation |
|---|---|---|
| Clock uncertainty costs more cadence than a few packet bytes | Dense group with ideal, NMEA and interrupted time sources | Declared versus actual clock error, missed slots, age and air use |
| A small local forwarding queue is sufficient | Single bridge and chain | Coverage and age of every source, bridge airtime and own updates |
| Weak-signal selection is worth more than fair rotation | Branches with controlled link quality | Useful deliveries and starvation, including the receiver behind only one candidate |
| Knowledge of neighbours pays for its control traffic | Dense group and moving or interrupted links | Total traffic including control, coverage and recovery time |
| More frequent short transmissions catch brief opportunities | A short contact window or bursty channel in a shared scenario | Useful source updates delivered within the window, not simply packet count |

The register is a research agenda. Only the current-v3 baseline is executable today; the
forwarding and record-selection hypotheses need implementations before the runner can measure
them. A known limitation of the baseline is still a useful result.

First run the baseline, then change one candidate or one environment assumption. Reject an
impossible byte, timing or per-node airtime budget before building a candidate. For each promising
result, vary the uncertain inputs over a stated range. A winner only under ideal clocks is a
conditional finding, not the selected protocol. Keep alternatives that offer a different useful
trade-off rather than hiding them behind a weighted score.

## The shared scenarios

The scenario describes source fixes, state changes and the environment. It does not decide which
node forwards a record. That is the candidate's job.

| Scenario | Environment | Question |
|---|---|---|
| Dense group | Ten nodes, all directly connected | What is the cost of forwarding when the direct link already suffices? |
| Single bridge | Four nodes in one bunch, five in another, one node linking both | Does the bridge serve every source without losing its own updates or exceeding its budget? |
| Chain | Five nodes hearing their neighbours | Do already-forwarded records traverse several hops, and how much age accumulates? |
| Branches | A source reaches two potential forwarders; a separate receiver can hear only one of them | Does suppression of a duplicate cut off a receiver that still needs that copy? |
| Partition and return | A middle link disappears and returns | Does delivery resume without an old state replacing a newer one? |

These are controlled cases, not claims of a particular range in metres. Add short contact windows,
one-way links, burst loss and timing errors to a relevant case when that is the hypothesis being
examined. Do not begin with the full product of every condition, PHY and protection mode.

Use the same source updates and environment timeline for candidates. Random conditions must not
change just because one candidate makes more calls to a random generator. Node clocks and radio
events remain model assumptions until measurements replace them. The first baseline's exact
assumptions belong in every run's metadata.

Topology and environment are separate inputs. A directed link means the scenario permits
reception; it is not a promise that a packet survives random loss or interference. Cutting links
can also describe a short contact window. Loss and interference do not turn the evaluator's
physical-path flag off: a path may exist while no packet gets through.

## Events and results

Run the real core and codec in the existing C simulator. The common environment handles the
transmitted bytes and their actual airtime. The candidate receives only data that its format
carries: the evaluator must not secretly supply a source revision, a missing timestamp or the
complete topology to fix a candidate's decisions.

Keep these distinct in the event log:

- the radio sender and the receiver;
- the author and identity of the original update;
- when its coordinates were measured;
- when the radio packet was sent, received and applied;
- later, when the phone received it and when the map displayed it.

A relay's new transmission does not create a new source update. A forwarded record does not
establish a direct radio link with its author. A run records the candidate, source revision and
working-tree status, scenario, seed, packet sizes, radio settings, clock assumptions and evidence
level. JSONL preserves events; a common summary produces `results.json` and `report.md`.

The executable input/output contract is enough for the present baseline: scenario and environment
files plus a seed go in; structured events and a summary come out. Future candidates must consume
the same source workload and model assumptions and expose their actual applied records. The
current runner does not yet select a different protocol executable. Add that selection when a
second implementation exists; no plugin ABI or replacement simulator is needed in advance.

### Correctness before performance

Check the observable contract before ranking performance:

- applying an old copy does not revive a cancelled state or make an old fix younger;
- source identity survives forwarding, and forwarding creates no false direct-hearing evidence;
- source updates do not circulate forever after they stop being generated;
- transmissions follow the candidate's access rules and per-node budget;
- for TDMA, the full actual packet fits its slot and the time-error gate remains in force.

A candidate without relaying can legitimately fail to deliver across a disconnected direct link.
Report the missing capability and the missing data; it is not a simulator error. Invariants about
a feature are not marked passed merely because that feature never ran. A security claim is checked
against the protection the candidate actually promises.

Each run names `checks_executed` and `checks_not_exercised`. The current baseline executes
`source_identity`, `applied_identity`, `update_order` and `timing_budget`. It does not exercise
`relayed_identity`, `relay_expiry`, `old_state_replay`, `fix_age_integrity`,
`full_slot_containment` or `rolling_duty_limit`. Zero reported failures is therefore not a pass
for those unexercised contracts. In particular, the core's timing-budget check does not measure
actual slot containment or independently verify the rolling duty ledger.

### Measures, without an invented pass score

| Measure | Report |
|---|---|
| Position age | Time-sampled ages per source–receiver pair, with median and p95 for known fixes; show missing and unknown-time samples separately |
| Gaps | The longest observed gap, including the unfinished final gap, per pair and worst receiver |
| State propagation | Delay to each receiver, plus not delivered and superseded-before-delivery counts |
| Availability | Share of observation time with no record, and separately with no physical path in the scenario |
| Air used | Every node's airtime and bytes, including control and relays; show the busiest node rather than just the group average |
| Timing limits | Assumed source error, drift and start latency, and skips due to insufficient time accuracy |
| Receive outcomes | Attempts split into accepted, unreachable, overlapping transmission, external interference, random loss and rejected by the core |
| Clock model accuracy | Largest actual UTC error at a transmission, beside the node's declared error; report bound exceedances separately from failures of the core's gate |

`age_samples` counts every observed source–receiver/time sample; `age_known_samples` counts the
finite known ages used for percentiles. Per-run percentiles use empirical nearest rank. Across
seeds the report gives the median of the per-run percentiles and the largest observed gap, not a
percentile pooled across runs. Raw results retain runs with no known samples.

`rx_attempts` includes every source-packet/receiver opportunity and equals the sum of
`rx_accepted`, `rx_unreachable`, `rx_interference`, `rx_external_interference`, `rx_loss` and
`rx_rejected`. These are model classifications, not measured RF failure diagnoses.
`clock_error_max_us` is the largest actual absolute clock error at transmission;
`clock_bound_exceeded` counts occasions exceeding the declared error. Such an exceedance exposes
the assumed bound; it is not automatically a failure of
the core's decision based on that bound.

Do not turn an unseen rider into an arbitrary enormous numeric age or drop it from the report.
Keep a negative measured age visible: a receiving clock can put a fix in its future. This is a
clock anomaly, not better freshness. State-delivery delays and elapsed gaps remain nonnegative.
Do not report state latency only for states that arrived. We carry the latest state: a transient
state that its author cancelled need not be delivered later as historical traffic. Distinguish
that case from a still-current state that never arrived.

The first run has no guessed acceptance threshold such as "p95 must be below N seconds" and no
weighted winner score. Compare the costs: equal freshness for less air, or better freshness for
the same budget. Report what a candidate does not support alongside its numbers.

## Two comparisons, three evidence levels

1. Hold PHY, source updates, clock assumptions and the environment fixed. For TDMA candidates a
   common frame can fit the largest packet. For TDMA against carrier sensing there is no shared
   frame: hold the source workload and airtime budget fixed instead.
2. Then give each candidate its feasible cadence and repeat the scenarios. This exposes the
   frequency paid for larger packets, relays and time guards.

| Evidence level | What runs | What it cannot establish |
|---|---|---|
| C simulation | The node and codec with a controlled air/clock model | Real RF, timing bounds, battery life or freshness on the phone's map |
| Android emulator bench | C nodes, the real app and OsmAnd, with modelled board BLE | Actual NimBLE/Zephyr BLE, full firmware restart or a real sleeping phone |
| Hardware | The selected boards and phones under recorded conditions | An untested group size or every terrain; two radios do not establish a relay chain |

The initial executable comparison stops at the receiving C node. Android scenarios and hardware
logs are later evidence, not automatically inherited from a simulation pass. The existing Android
bench also needs controlled scenarios before it can run the bridge and chain cases. See its
[boundary table](../tools/bench/README.md) and the [hardware procedure](BENCH.md).

Most logical cases can be simulated before the boards arrive. The point of hardware is to
calibrate the uncertainty envelope and check that the real drivers and platforms follow the
modelled contract. Measure GNSS/NMEA delay, start latency, drift, loss and power under recorded
conditions; replace or bracket the corresponding assumptions, then rerun the scenarios. A
synthetic NMEA profile does not become a measurement of an L1 because its numbers look plausible.

The radio model has no measured fading, capture effect or propagation prediction. Overlapping
heard transmissions lose their packets; its external-interference profile is a periodic erasure
process. It does not implement or validate real carrier sensing. The comparison suite uses a
common PHY and cannot establish reception between incompatible settings or predict which mode
reaches further. Short windows, one-way links, clock drift and burst interference remain useful
controlled inputs without being claimed as physical truth.

Two radios suffice for direct exchange and timing measurements. A physical `A–B–C` forwarding
test needs at least three radios and evidence that `A–C` was not received directly. For holdover,
stop time corrections: merely losing a position fix may leave GNSS time available.

## First runnable baseline

Start with the current v3 on the controlled scenarios in `tools/comparison/`. Each lasts 120 s,
with 20 s of warm-up and a 4 s frame. The default seeds are 17, 29 and 43. These are reproducible
baseline settings, not field profiles. Current v3 cannot relay, so the bridge, chain and branch
cases are expected to expose missing records. Add each later candidate only when it can produce
the same observations from its actual implementation.

Two suites answer different questions:

| Suite | Runs with the defaults | Purpose |
|---|---|---|
| `topology` | Five scenarios, ideal environment, three seeds: 15 runs | Establish direct-reception coverage and the cost of the baseline |
| `sensitivity` | Dense and bridge scenarios, six environments, three seeds: 36 runs | See how loss and non-ideal time assumptions change that same implementation |

The environments are `ideal`, `random-loss`, `bursty-channel`, `nmea-clock`, `gnss-holdover` and
`fix-loss`, stored in `tools/comparison/environments/`. `ideal` has PPS on every node, ideal
clocks and no random loss. The NMEA example assumes a 40 ppm drift range, one NMEA-only node,
120 ms arrival lag with 5 ms jitter, a told lag of 100 ms and an error bound of 25 ms. Those
numbers are illustrative, not an L1 calibration. The two outage profiles act from 30 to 90 s on
all nodes: `gnss-holdover` stops time corrections as well as fixes; `fix-loss` keeps GNSS time
while invalidating position fixes. A bound that lasts beyond the standard run can be examined
with a longer scenario; this suite does not claim the complete safe holdover time.

From the repository root:

```sh
python3 tools/compare.py --output /tmp/convoy-comparison
# Vary the environment for the same baseline:
python3 tools/compare.py --suite sensitivity --output /tmp/convoy-sensitivity
# One case or another repeatable seed set:
python3 tools/compare.py --scenario bridge-10 --seeds 17,29,43 --output /tmp/convoy-bridge
# Select environments explicitly:
python3 tools/compare.py --scenario bridge-10 --environment ideal --environment nmea-clock --output /tmp/convoy-clocks
# The same runner through make:
make -C firmware compare ARGS="--output /tmp/convoy-comparison-other"
```

`--scenario` and `--environment` may be repeated and override that part of the selected suite.
The default suite is `topology`. The output directory must be new or empty so a previous result
is not overwritten. Omitting `--output` uses a temporary directory. The directory holds JSONL run
events, scenario and environment snapshots, `results.json` and `report.md`. Keep generated runs
outside the committed source tree. Save a result's complete settings with it before using it to
support a protocol decision.

### Scenario files

The C entry point is `firmware/build/host/sim --compare PATH SEED [ENV_FILE]`. Without the optional
environment file it uses ideal clocks and the loss setting from the scenario. The small line-based
input keeps the initial runner independent of a new parsing dependency:

```text
name example-3
nodes 3
interval_ms 4000
duration_s 120
warmup_s 20
loss_permille 0
link 0 1
link 1 0
link 1 2
link 2 1
help 30 0 1
help 50 0 0
cut 60 90 1 2
cut 60 90 2 1
```

`name`, `nodes`, `interval_ms`, `duration_s` and `warmup_s` are required. `loss_permille` is
optional, from 0 to 1000. The graph starts empty. `link FROM TO` adds one directed link;
`clique FIRST LAST` adds every link in both directions in that inclusive node range. `cut FROM_S
UNTIL_S FROM TO` temporarily removes one directed link. `help AT_S SOURCE 0|1` changes the
source's current v3 help state. A bidirectional interruption needs both cuts. This baseline
vocabulary does not yet expose arbitrary movement or the full v4 state set.

### Environment files

An environment file requires `environment SLUG`. Its optional clock directives are `drift_ppm`,
`nmea_only`, `nmea_lag_us`, `nmea_jitter_us`, `nmea_told_lag_us` and `nmea_error_us`; omitted
values keep the existing baseline: no drift, PPS on every node, NMEA lag 100 ms without jitter,
and NMEA configured with zero lag and a 100 ms error bound. `loss_permille` overrides the scenario's loss value.
`nmea_only` selects that many nodes, starting at node zero. Lag plus or minus jitter must stay
within the model's single-second arrival window.

```text
environment example-clock-outage
drift_ppm 40
nmea_only 1
nmea_lag_us 120000
nmea_jitter_us 5000
nmea_told_lag_us 100000
nmea_error_us 25000
gnss_outage 30 90 all
```

`gnss_outage FROM_S UNTIL_S NODE_OR_all` suppresses PPS and NMEA time corrections. The same syntax
with `fix_outage` makes coordinates invalid while time and PPS continue. `interference PERIOD_MS
BUSY_MS` describes a shared periodic busy interval with seed-dependent phase: any overlapping
packet is lost. It does not remove graph links. These directives describe test inputs; their
values are not estimates inferred by a candidate or measurements of a real board.

The event types are `run`, `tx`, `rx`, `applied`, `sample`, `state` and `summary`. The report's
known-fix age percentiles are conditional on known age; unseen records and unknown clock samples
are separate fractions. State outcomes account for every expected source-to-receiver delivery as
delivered, superseded or pending, with latency computed only for delivered updates. They measure
state arrival, not the v3 lists of automatic answers. Physical-path availability is an evaluator's
observation of the directed scenario graph, not information given to the node.

Inputs accept 2–32 nodes, intervals of 100–60000 ms, durations of 1–3600 s and warm-up shorter
than the duration; seeds are nonzero 32-bit integers. Help changes must alternate per source
from its initial normal state, and cuts must name existing directed links. Malformed scenarios
are refused rather than partly applied. The comparison's C checks are included in
`make -C firmware test`.
