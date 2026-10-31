#!/usr/bin/env python3
"""Run the firmware core on repeatable scenarios and keep its evidence together."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = Path(__file__).resolve().parent / "comparison"
ENVIRONMENTS = SCENARIOS / "environments"
SIM = ROOT / "firmware/build/host/sim"
SUITES = {
    "topology": (["branches-4", "bridge-10", "chain-5", "dense-10", "partition-5"], ["ideal"]),
    "sensitivity": (["dense-10", "bridge-10"],
                    ["ideal", "random-loss", "bursty-channel", "nmea-clock", "gnss-holdover", "fix-loss"]),
}
RX_COUNTS = ("rx_accepted", "rx_unreachable", "rx_interference", "rx_external_interference",
             "rx_loss", "rx_rejected")
COUNTS = ("age_samples", "age_known_samples", "unseen_samples", "clock_unknown_samples", "state_expected",
          "state_delivered", "state_superseded", "state_pending", "state_delay_samples",
          "contract_failures", "tx_count", "tx_unsure", "rx_attempts", *RX_COUNTS,
          "clock_bound_exceeded")
FRACTIONS = ("unseen_fraction", "path_unavailable_fraction", "clock_unknown_fraction",
             "tx_airtime_max_fraction")
DELAYS = ("age_p50_ms", "age_p95_ms", "state_delay_p50_ms", "state_delay_p95_ms")


def seeds(value):
    try:
        result = [int(s) for s in value.split(",")]
        if not result or len(set(result)) != len(result) or any(not 1 <= n <= 0xffffffff for n in result):
            raise ValueError
        return result
    except ValueError:
        raise argparse.ArgumentTypeError("seeds must be distinct positive 32-bit integers") from None


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True,
                          capture_output=True, text=True).stdout.strip()


def reject_constant(value):
    raise ValueError(f"non-finite JSON number: {value}")


def read_run(path, scenario, seed, environment=None):
    """A missing or contradictory result must not turn into a successful comparison."""
    run = summary = None
    last_event = None
    with path.open() as stream:
        for line_number, line in enumerate(stream, 1):
            try:
                event = json.loads(line, parse_constant=reject_constant)
                if not isinstance(event, dict) or not isinstance(event.get("event"), str):
                    raise ValueError("expected an event object")
                if summary is not None:
                    raise ValueError("event after summary")
                if event["event"] == "run":
                    if run is not None or last_event is not None:
                        raise ValueError("run must be the first and only run event")
                    run = event
                elif run is None:
                    raise ValueError("missing run event")
                if event["event"] == "summary":
                    summary = event
                last_event = event["event"]
            except ValueError as error:
                raise ValueError(f"{path.name}:{line_number}: {error}") from error
    if run is None or summary is None:
        raise ValueError(f"{path.name}: missing run or summary")
    if run.get("schema") != 1 or run.get("scenario") != scenario or run.get("seed") != seed:
        raise ValueError(f"{path.name}: run does not match the requested scenario and seed")
    if environment is not None and run.get("environment") != environment:
        raise ValueError(f"{path.name}: run does not match the requested environment")
    if not isinstance(run.get("candidate"), str) or not run["candidate"]:
        raise ValueError(f"{path.name}: missing candidate")
    for key in COUNTS:
        value = summary.get(key)
        if type(value) is not int or value < 0:
            raise ValueError(f"{path.name}: invalid {key}")
    if any(summary[key] > summary["age_samples"] for key in
           ("age_known_samples", "unseen_samples", "clock_unknown_samples")):
        raise ValueError(f"{path.name}: sample counts exceed the total")
    if summary["state_expected"] != sum(summary[k] for k in
                                         ("state_delivered", "state_superseded", "state_pending")):
        raise ValueError(f"{path.name}: state outcomes do not account for every receiver")
    if summary["state_delay_samples"] != summary["state_delivered"]:
        raise ValueError(f"{path.name}: state delay samples do not match delivered states")
    if summary["rx_attempts"] != sum(summary[key] for key in RX_COUNTS):
        raise ValueError(f"{path.name}: reception outcomes do not account for every attempt")
    for key in FRACTIONS:
        value = summary.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 1:
            raise ValueError(f"{path.name}: invalid {key}")
    for key in (*DELAYS, "gap_max_ms", "clock_error_max_us"):
        value = summary.get(key)
        if value is None and key in DELAYS:
            continue
        if type(value) not in (int, float) or not math.isfinite(value) or (
                value < 0 and key not in ("age_p50_ms", "age_p95_ms")):
            raise ValueError(f"{path.name}: invalid {key}")
    for count, keys in (("age_known_samples", ("age_p50_ms", "age_p95_ms")),
                        ("state_delay_samples", ("state_delay_p50_ms", "state_delay_p95_ms"))):
        if any((summary[k] is None) != (summary[count] == 0) for k in keys):
            raise ValueError(f"{path.name}: {count} and its percentiles disagree")
    return run, summary


def aggregate(runs):
    rows = []
    def group(run):
        return run["candidate"], run["scenario"], run.get("environment", "ideal")
    for candidate, scenario, environment in sorted({group(r) for r in runs}):
        summaries = [r["summary"] for r in runs if group(r) == (candidate, scenario, environment)]
        row = {"candidate": candidate, "scenario": scenario, "environment": environment, "runs": len(summaries)}
        for key in (*DELAYS, *FRACTIONS):
            known = [s[key] for s in summaries if s[key] is not None]
            row[key] = statistics.median(known) if known else None
            if len(known) != len(summaries):
                row[f"{key}_runs_without_samples"] = len(summaries) - len(known)
        row["gap_max_ms"] = max(s["gap_max_ms"] for s in summaries)
        row["clock_error_max_us"] = max(s["clock_error_max_us"] for s in summaries)
        row["tx_airtime_max_fraction"] = max(s["tx_airtime_max_fraction"] for s in summaries)
        for key in COUNTS:
            row[key] = sum(s[key] for s in summaries)
        rows.append(row)
    return rows


def display(value, scale=1, suffix=""):
    return "—" if value is None else f"{value * scale:.2f}{suffix}"


def report(data):
    executed = set.intersection(*(set(run["run"].get("checks_executed", [])) for run in data["runs"]))
    unexercised = set().union(*(run["run"].get("checks_not_exercised", []) for run in data["runs"]))
    lines = ["# Protocol comparison", "", "Evidence: **C simulation**, measured at receiving nodes, not on a phone or map.",
             "The current candidate is the unmodified v3 protocol, which has no relaying.",
             "Low coverage in a bridge or chain is a result, not a runner failure.", "",
             f"Revision: `{data['source']['revision']}`; working tree: "
             f"{'modified' if data['source']['dirty'] else 'clean'}.",
             f"Simulator SHA-256: `{data['source']['simulator_sha256']}`.", "",
             "| Candidate / scenario | Environment | Runs | Fix age p95, s | Longest observed gap, s | Unseen | No path | States delivered / expected | Superseded / pending | Busiest node | Contract failures |",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in data["scenarios"]:
        lines.append(f"| {row['candidate']} / {row['scenario']} | {row['environment']} | {row['runs']} | "
                     f"{display(row['age_p95_ms'], .001)} | {display(row['gap_max_ms'], .001)} | "
                     f"{display(row['unseen_fraction'], 100, '%')} | "
                     f"{display(row['path_unavailable_fraction'], 100, '%')} | "
                     f"{row['state_delivered']} / {row['state_expected']} | "
                     f"{row['state_superseded']} / {row['state_pending']} | "
                     f"{display(row['tx_airtime_max_fraction'], 100, '%')} | {row['contract_failures']} |")
    lines += ["", "Percentiles and fractions are medians over the repeated runs; the gap and busiest-node airtime are the largest observed.",
              "Each run uses empirical nearest-rank percentiles: sorted sample at ceil(p × sample count), with ranks starting at one.",
              "Fix ages include only known coordinates with usable time; unseen and clock-unknown samples stay separate.",
              "Negative fix ages expose a receiving-clock error that puts the fix in the future; they are not improved freshness.",
              "A dash means no samples, not zero delay. Raw per-run results retain missing-percentile runs.",
              "Superseded states changed at the source before the receiver saw them; pending states were still unseen at the end.",
              "No path means no directed path in the scenario at that moment, not a protocol delivery guarantee.",
              "Contract failures count only the observed checks listed below; zero does not validate the whole feature registry.", "",
              "## Sensitivity to the environment", "",
              "Different environments are separate results, never pooled into a candidate's score. Compare candidates within the same environment first.",
              "Busy windows model shared external interference; they do not remove a physical path or act as a simulated neighbouring protocol.",
              "Clock error is measured against the simulator's true time at transmission. The node's own declared error is a separate estimate.",
              "A bound exceedance is model evidence of an optimistic estimate, not an extra protocol pass/fail rule.", "",
              "| Scenario / environment | RX accepted / attempted | Out of reach | Internal overlap / half duplex | External interference | Random loss | Rejected by node | Largest clock error, ms | TX outside declared clock bound | Uncertain-time skips |",
              "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in data["scenarios"]:
        lines.append(f"| {row['scenario']} / {row['environment']} | {row['rx_accepted']} / {row['rx_attempts']} | "
                     f"{row['rx_unreachable']} | {row['rx_interference']} | {row['rx_external_interference']} | "
                     f"{row['rx_loss']} | {row['rx_rejected']} | {display(row['clock_error_max_us'], .001)} | "
                     f"{row['clock_bound_exceeded']} | {row['tx_unsure']} |")
    lines += ["", "Receptions use the first applicable outcome: unreachable, internal overlap, external interference, random loss, then core acceptance/rejection.",
              "Outcome counts and skips are summed over runs; the clock error is the largest observed. An attempt is one transmitted packet paired with one other node, not an independent channel trial.", "",
              "## Checks", "",
              "Reported as executed in every run: " + (", ".join(f"`{name}`" for name in sorted(executed)) or "none declared") + ".",
              "Reported as not exercised in at least one run: " +
              (", ".join(f"`{name}`" for name in sorted(unexercised)) or "none declared") + ".", "",
              "## Assumptions", ""]
    lines.extend(f"- {assumption}" for assumption in data["assumptions"])
    lines += ["", "## Per-run state delay and clock visibility", "",
              "| Scenario / environment / seed | State delay p95, s | Clock unknown | Raw events |",
              "|---|---:|---:|---|"]
    for run in data["runs"]:
        summary = run["summary"]
        lines.append(f"| {run['scenario']} / {run['environment']} / {run['seed']} | {display(summary['state_delay_p95_ms'], .001)} | "
                     f"{display(summary['clock_unknown_fraction'], 100, '%')} | [{run['events']}]({run['events']}) |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    available = {p.stem: p for p in sorted(SCENARIOS.glob("*.scenario"))}
    environments = {p.stem: p for p in sorted(ENVIRONMENTS.glob("*.environment"))}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="new or empty directory; defaults to a fresh temporary directory")
    parser.add_argument("--suite", choices=SUITES, default="topology",
                        help="topology: five ideal cases; sensitivity: dense and bridge across six environments")
    parser.add_argument("--scenario", choices=available, action="append", help="repeat to override the suite's scenarios")
    parser.add_argument("--environment", choices=environments, action="append", help="repeat to override the suite's environments")
    parser.add_argument("--seeds", type=seeds, default=[17, 29, 43])
    args = parser.parse_args(argv)
    if not available:
        parser.error("no comparison scenarios found")
    default_scenarios, default_environments = SUITES[args.suite]
    selected_scenarios = list(dict.fromkeys(args.scenario or default_scenarios))
    selected_environments = list(dict.fromkeys(args.environment or default_environments))
    if any(name not in available for name in selected_scenarios) or any(name not in environments for name in selected_environments):
        parser.error("suite fixture is missing")
    if args.output is not None and args.output.exists() and (not args.output.is_dir() or any(args.output.iterdir())):
        parser.error("output must be a new or empty directory; previous evidence is not overwritten")
    subprocess.run(["make", "-C", str(ROOT / "firmware"), "build/host/sim"], check=True)
    output = args.output.resolve() if args.output is not None else Path(tempfile.mkdtemp(prefix="convoy-comparison-"))
    output.mkdir(parents=True, exist_ok=True)
    (output / "scenarios").mkdir()
    (output / "environments").mkdir()
    status = git("status", "--porcelain")
    source = {"revision": git("rev-parse", "HEAD"), "dirty": bool(status), "status": status,
              "simulator_sha256": digest(SIM)}
    assumptions = ["Actual firmware node core and codec; no SX1262 driver, platform task or phone transport is executed.",
                   "Synthetic directed links, configured clock models, loss and busy windows; no measured propagation, fading or calibrated board timing.",
                   "Node-local fixed crystal drift; NMEA jitter and interference phase are seeded. The explicit environment fixture and run metadata define each case.",
                   "Any overlapping audible packet destroys reception: no capture effect, SNR model or PHY isolation. RSSI is constant and the radio starts immediately without driver latency.",
                   "SF7/250, explicit header, PHY CRC enabled, preamble 8; airtime is calculated from real serialized bytes.",
                   "The v3 help flag is observed as a state; its automatic acknowledgements still consume airtime.",
                   "Neither board Bluetooth, power, phone delivery nor OsmAnd rendering is measured."]
    runs = []
    scenario_hashes = {}
    environment_hashes = {}
    for environment in selected_environments:
        snapshot = output / "environments" / environments[environment].name
        snapshot.write_bytes(environments[environment].read_bytes())
        environment_hashes[environment] = digest(snapshot)
    for name in selected_scenarios:
        snapshot = output / "scenarios" / available[name].name
        snapshot.write_bytes(available[name].read_bytes())
        scenario_hashes[name] = digest(snapshot)
        for environment in selected_environments:
            environment_path = output / "environments" / environments[environment].name
            for seed in args.seeds:
                events = output / f"{name}-{environment}-seed-{seed}.jsonl"
                if digest(SIM) != source["simulator_sha256"]:
                    raise ValueError("simulator changed during comparison; run again into a new directory")
                with events.open("w") as stream:
                    result = subprocess.run([str(SIM), "--compare", str(snapshot), str(seed), str(environment_path)],
                                            stdout=stream, stderr=subprocess.PIPE, text=True)
                if result.returncode not in (0, 1):
                    raise ValueError(f"simulator failed for {name}/{environment}/{seed}; events: {events}\n{result.stderr.strip()}")
                run, summary = read_run(events, name, seed, environment)
                if result.returncode == 1 and not summary["contract_failures"]:
                    raise ValueError(f"simulator failed without a contract result for {name}/{environment}/{seed}; events: {events}")
                runs.append({"candidate": run["candidate"], "scenario": name, "environment": environment, "seed": seed,
                             "events": events.name, "run": run, "summary": summary})
    data = {"schema": 1, "evidence": "simulation", "source": source, "seeds": args.seeds,
            "scenario_sha256": scenario_hashes, "environment_sha256": environment_hashes, "assumptions": assumptions,
            "runs": runs, "scenarios": aggregate(runs)}
    (output / "results.json").write_text(json.dumps(data, indent=2, allow_nan=False) + "\n")
    (output / "report.md").write_text(report(data))
    print(f"Comparison: {len(runs)} runs; {output / 'report.md'}")
    return 1 if any(run["summary"]["contract_failures"] for run in runs) else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"compare: {error}", file=sys.stderr)
        sys.exit(1)
