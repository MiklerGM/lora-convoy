#!/usr/bin/env python3
"""Compare actual C protocol cores on reproducible DES worlds and inspect offline replay."""
import argparse
import copy
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile

from simulator.native import NativeLibrary, ROOT
from simulator.replay import write_replay
from simulator.runtime import Simulation, digest, keyed

SCENARIOS = Path(__file__).resolve().parent / "simulation"


def pairs_object(items):
    result = {}
    for name, value in items:
        if name in result:
            raise ValueError(f"duplicate JSON property {name}")
        result[name] = value
    return result


def load(path):
    def invalid(value):
        raise ValueError(f"non-finite JSON value {value}")
    return json.loads(path.read_text(), object_pairs_hook=pairs_object, parse_constant=invalid)


def numbers(value):
    try:
        result = [int(item) for item in value.split(",")]
    except ValueError:
        result = []
    if not result or len(set(result)) != len(result) or any(not 0 <= n < 2**64 for n in result):
        raise argparse.ArgumentTypeError("seeds must be distinct unsigned 64-bit integers")
    return result


def generated(kind, size):
    if not 2 <= size <= 32:
        raise ValueError("generated groups require 2..32 participants")
    groups = 2 if kind == "neighbours" else 1
    nodes = [{"id": f"g{group}n{i}", "group": f"g{group}", "slot": i,
              "interval_ms": max(4000, (size+1)*300), "listen_one_in": 32,
              "trajectory": [{"t_s":0,"x_m":i*100,"y_m":group*50},
                             {"t_s":120,"x_m":i*100+1200,"y_m":group*50}]}
             for group in range(groups) for i in range(size)]
    result = {"schema":1, "name":f"{kind}-{size}", "duration_s":120, "warmup_s":20,
              "nodes":nodes, "actions":[{"id":"help-on","t_s":30,"node":"g0n0","kind":"state","state":"help"},
                                        {"id":"help-off","t_s":50,"node":"g0n0","kind":"state","state":"normal"}]}
    if kind in ("dense", "neighbours"):
        result["default_link"] = {"decode":True,"sense":True,"interfere":True,"power_dbm":-90}
    else:
        links=[]
        for i in range(size):
            for j in range(size):
                connected = abs(i-j)==1 if kind=="chain" else (i==size//2 or j==size//2 or
                               (i<size//2 and j<size//2) or (i>size//2 and j>size//2))
                if i!=j and connected:
                    links.append({"from":f"g0n{i}","to":f"g0n{j}","power_dbm":-90})
        result["links"]=links
    return result


def prepare(config, candidate, layout_seed, shuffle_slots, action_phase_s, interval_ms):
    config = copy.deepcopy(config)
    groups = {}
    for node in config["nodes"]:
        node["candidate"] = candidate
        if interval_ms is not None:
            node["interval_ms"] = interval_ms
        groups.setdefault(node["group"], []).append(node)
    for group, nodes in groups.items():
        if shuffle_slots:
            slots = sorted(node["slot"] for node in nodes)
            random.Random(keyed(layout_seed,"slots",group)).shuffle(slots)
            for node,slot in zip(sorted(nodes,key=lambda x:x["id"]),slots):
                node["slot"] = slot
    if action_phase_s:
        for action in config.get("actions", []):
            shift = int.from_bytes(keyed(layout_seed,"action-phase",action["node"])[:8],"big")/2**64*action_phase_s
            action["t_s"] += shift
            if action["t_s"] >= config["duration_s"]:
                raise ValueError("action phase moves an action beyond the run; extend duration")
    return config


def world_manifest(world):
    result = copy.deepcopy(world.config)
    for node in result["nodes"]:
        for name in ("candidate","interval_ms","duty_permille","listen_one_in","relay_records","relay_ttl_s", "phy", "tx_power_dbm"):
            node.pop(name,None)
    return {"environment_seed":world.env_seed,"layout_seed":world.layout_seed,"world":result}


def git(*args):
    return subprocess.run(["git","-C",str(ROOT),*args],check=True,capture_output=True,text=True).stdout.strip()


def markdown(data):
    lines=["# DES protocol comparison", "", "Evidence: actual C node and codec with a synthetic Python DES world. No physical-radio or phone claim.",
           "The relay candidate is a research framing/forwarding implementation, not the proposed v4 format or sdk firmware.", "",
           "| Scenario | Candidate | Seed | True age p95, s | Known pair-time | Busiest node RF, % | Violations |",
           "|---|---|---:|---:|---:|---:|---:|"]
    for run in data["runs"]:
        summary=run["summary"]
        age=summary["metrics"]["overall"]["true_age"]["p95_ns"]
        coverage=summary["metrics"]["overall"]["coverage_fraction"]
        air=max((row["airtime_ns"] for row in summary["airtime"]),default=0)/summary["metrics"]["duration_ns"]*100
        lines.append(f"| {run['scenario']} | {run['candidate']} | {run['seed']} | "
                     f"{'—' if age is None else f'{age/1e9:.4f}'} | {'—' if coverage is None else f'{coverage:.2%}'} | {air:.3f} | {len(summary['contract_failures'])} |")
    lines += ["", "True ages are weighted by elapsed time. Coverage includes unknown pairs; a good conditional percentile alone does not establish group coverage.",
              "RF fraction is over this observation window; compliance checks use each node's actual rolling-hour cost and stated budget.",
              "Violations count only declared runtime checks: native source identity, transmission time availability and actual rolling-hour RF cost. Zero is not a pass of the entire architecture or hardware model.",
              "Native minute-bucket booked cost is reported separately. The decoder treats failed reception as erasure; bit corruption and recapture are unsupported.",
              "Reported clock ages use a piecewise affine calendar between observations, with the declared native tick-resolution error bound.", "",
              "## Paired differences", "", "Each pair uses the same environment and layout realization. These are descriptive independent-run differences, not packet-level confidence intervals."]
    for row in data["paired"]:
        differences=row["coverage_differences"]
        age=row["true_p95_differences_s"]
        lines += [f"- {row['scenario']}: {row['candidate']} minus {row['baseline']}, {row['worlds']} paired worlds; "
                  f"mean coverage difference {'—' if not differences else f'{statistics.mean(differences):+.2%}'}; "
                  f"mean known-age p95 difference {'—' if not age else f'{statistics.mean(age):+.4f} s'}. "
                  "Coverage and age must be assessed together."]
    lines += ["", "## Individual traces", ""]
    for run in data["runs"]:
        folder=run["directory"]
        lines.append(f"- {run['scenario']} / {run['candidate']} / {run['seed']}: "
                     f"[summary]({folder}/summary.json), [events]({folder}/events.jsonl)"+
                     (f", [replay]({folder}/replay.html)" if run["replay"] else "")+".")
    lines += ["", "## Evidence boundaries", "", "Parameters are synthetic unless a scenario identifies measured provenance. "
              "No DEM, calibrated RF, real Bluetooth/map delivery, battery model, SDK firmware or Meshtastic adapter is executed. "
              "Finite seeds describe selected conditions; they do not establish universal superiority."]
    return "\n".join(lines)+"\n"


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario",action="append",help="fixture name or JSON path; default dense-10")
    parser.add_argument("--suite",action="store_true",help="run all checked-in scenarios")
    parser.add_argument("--generate",choices=("dense","chain","bridge","neighbours"))
    parser.add_argument("--size",type=int,default=10)
    parser.add_argument("--candidates",default="v3,relay",help="comma-separated v3 and/or relay")
    parser.add_argument("--seeds",type=numbers,default=[17,29,43])
    parser.add_argument("--shuffle-slots",action="store_true")
    parser.add_argument("--action-phase-s",type=float,default=0)
    parser.add_argument("--interval-ms",type=int)
    parser.add_argument("--reverse-ties",action="store_true")
    parser.add_argument("--reference",action="store_true",help="compute causal unlimited-content contact diagnostic")
    parser.add_argument("--no-replay",action="store_true")
    parser.add_argument("--output",type=Path)
    args=parser.parse_args(argv)
    candidates=args.candidates.split(",")
    if not candidates or len(set(candidates))!=len(candidates) or any(c not in ("v3","relay") for c in candidates):
        parser.error("candidates must be distinct v3 and/or relay")
    if not 0<=args.action_phase_s<1e6:
        parser.error("action phase must be finite and nonnegative")
    if args.output is not None and args.output.exists() and (not args.output.is_dir() or any(args.output.iterdir())):
        parser.error("output must be new or empty; previous evidence is not overwritten")
    if sum((bool(args.scenario),args.suite,bool(args.generate)))>1:
        parser.error("choose --scenario, --suite or --generate")
    if args.generate:
        scenarios=[generated(args.generate,args.size)]
    else:
        paths=sorted(SCENARIOS.glob("*.json")) if args.suite else [Path(name) if Path(name).is_file() else SCENARIOS/(name+".json")
                                                               for name in (args.scenario or ["dense-10"])]
        scenarios=[load(path) for path in paths]
    library=NativeLibrary()
    output=args.output.resolve() if args.output is not None else Path(tempfile.mkdtemp(prefix="convoy-des-"))
    output.mkdir(parents=True,exist_ok=True)
    source={"revision":git("rev-parse","HEAD"),"status":git("status","--porcelain"),"native_sha256":library.sha256,
            "cli_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            "model_sha256":digest({p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((ROOT/"tools/simulator").glob("*.py"))})}
    runs=[]
    hashes={}
    for scenario in scenarios:
        for seed in args.seeds:
            env=int.from_bytes(keyed(seed,"environment")[:8],"big")
            layout=int.from_bytes(keyed(seed,"layout")[:8],"big")
            for candidate in candidates:
                prepared=prepare(scenario,candidate,layout,args.shuffle_slots,args.action_phase_s,args.interval_ms)
                protocol=int.from_bytes(keyed(seed,"protocol",candidate)[:8],"big")
                sim=Simulation(prepared,environment_seed=env,layout_seed=layout,
                               protocol_seed=protocol,
                               reverse_ties=args.reverse_ties,library=library)
                try:
                    name=sim.world.config["name"]
                    folder=output/f"{name}-{candidate}-{seed}"
                    folder.mkdir()
                    world_hash=digest(world_manifest(sim.world))
                    pair=(name,seed)
                    if pair in hashes and hashes[pair]!=world_hash:
                        raise ValueError("paired candidates did not receive the same world")
                    hashes[pair]=world_hash
                    summary=sim.run(args.reference)
                    summary["run"]={"candidate":candidate,"seed":seed,"environment_seed":env,"layout_seed":layout,
                                    "protocol_seed":protocol,"reverse_ties":args.reverse_ties,
                                    "world_sha256":world_hash,"scenario_sha256":digest(sim.world.config),"source":source}
                    (folder/"scenario.json").write_text(json.dumps(sim.world.config,indent=2,allow_nan=False)+"\n")
                    (folder/"summary.json").write_text(json.dumps(summary,indent=2,allow_nan=False)+"\n")
                    with (folder/"events.jsonl").open("w") as stream:
                        stream.write(json.dumps({"event":"run","t_ns":0,**summary["run"]},allow_nan=False)+"\n")
                        for event in sim.events:
                            stream.write(json.dumps(event,allow_nan=False)+"\n")
                    if not args.no_replay:
                        write_replay(sim.events,folder/"replay.html",sim.world.config)
                    runs.append({"scenario":name,"candidate":candidate,"seed":seed,"directory":folder.name,
                                 "replay":not args.no_replay,"summary":summary})
                finally:
                    sim.close()
    paired=[]
    for name in sorted({run["scenario"] for run in runs}):
        for candidate in candidates[1:]:
            coverage,ages=[],[]
            for seed in args.seeds:
                row={r["candidate"]:r["summary"]["metrics"]["overall"] for r in runs if r["scenario"]==name and r["seed"]==seed}
                a,b=row[candidates[0]],row[candidate]
                if a["coverage_fraction"] is not None and b["coverage_fraction"] is not None:
                    coverage.append(b["coverage_fraction"]-a["coverage_fraction"])
                if a["true_age"]["p95_ns"] is not None and b["true_age"]["p95_ns"] is not None:
                    ages.append((b["true_age"]["p95_ns"]-a["true_age"]["p95_ns"])/1e9)
            paired.append({"scenario":name,"baseline":candidates[0],"candidate":candidate,
                           "worlds":len(args.seeds),
                           "coverage_differences":coverage,"true_p95_differences_s":ages})
    data={"schema":1,"runs":runs,"paired":paired,"source":source}
    (output/"results.json").write_text(json.dumps(data,indent=2,allow_nan=False)+"\n")
    (output/"report.md").write_text(markdown(data))
    print(f"DES comparison: {len(runs)} runs; {output/'report.md'}")
    return int(any(run["summary"]["contract_failures"] for run in runs))


if __name__=="__main__":
    try:
        sys.exit(main())
    except (ValueError,RuntimeError,OSError,subprocess.CalledProcessError) as error:
        print(f"simulate: {error}",file=sys.stderr)
        sys.exit(1)
