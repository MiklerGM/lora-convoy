#!/usr/bin/env python3
"""Compare actual C protocol cores on reproducible DES worlds and inspect offline replay."""
import argparse
import copy
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys
import tempfile

from simulator.native import NativeLibrary, ROOT
from simulator.replay import write_replay
from simulator.runtime import Simulation, digest, keyed
from simulator.types import Phy

SCENARIOS = Path(__file__).resolve().parent / "simulation"
IMPLEMENTATIONS = ("v3", "relay", "v3-cad", "relay-cad")


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


def candidate_setups(path):
    if path is None:
        return {}
    data=load(path)
    if (not isinstance(data,dict) or set(data)!={"schema","candidates"} or
            type(data["schema"]) is not int or data["schema"]!=1 or
            not isinstance(data["candidates"],dict) or not data["candidates"]):
        raise ValueError("setups require schema 1 and a nonempty candidates object")
    integer_bounds={"interval_ms":(100,60000),"duty_permille":(1,1000),
                    "listen_one_in":(0,65535),"relay_records":(0,3)}
    number_bounds={"relay_ttl_s":(0,86400),"tx_power_dbm":(-20,30)}
    allowed={*integer_bounds,*number_bounds,"phy"}
    for label,definition in data["candidates"].items():
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,31}",label):
            raise ValueError("setup labels require 1..32 ASCII letters/digits, '.', '_' or '-', starting with a letter/digit")
        if (not isinstance(definition,dict) or set(definition)!={"implementation","settings"} or
                definition["implementation"] not in IMPLEMENTATIONS or not isinstance(definition["settings"],dict)):
            raise ValueError(f"{label}: setup requires a built-in implementation and settings object")
        if label in IMPLEMENTATIONS and label!=definition["implementation"]:
            raise ValueError(f"{label}: a built-in label cannot identify another implementation")
        settings=definition["settings"]
        if set(settings)-allowed:
            raise ValueError(f"{label}: unsupported setup settings {sorted(set(settings)-allowed)}")
        for key,(low,high) in integer_bounds.items():
            if key in settings and (type(settings[key]) is not int or not low<=settings[key]<=high):
                raise ValueError(f"{label}: {key} must be an integer in {low}..{high}")
        for key,(low,high) in number_bounds.items():
            if key in settings:
                value=settings[key]
                if (type(value) not in (int,float) or not low<=value<=high or
                        (key=="relay_ttl_s" and value==0)):
                    raise ValueError(f"{label}: invalid {key}")
        if "phy" in settings:
            phy=settings["phy"]
            if not isinstance(phy,dict) or set(phy)-Phy.__dataclass_fields__.keys():
                raise ValueError(f"{label}: phy must contain supported PHY fields only")
            Phy(**phy)
    return data["candidates"]


def prepare(config, candidate, layout_seed, shuffle_slots, action_phase_s, interval_ms, settings=None):
    config = copy.deepcopy(config)
    groups = {}
    for node in config["nodes"]:
        node["candidate"] = candidate
        for key,value in (settings or {}).items():
            if key=="phy":
                if not isinstance(node.get("phy",{}),dict):
                    raise ValueError("scenario node phy must be an object")
                node[key] = node.get("phy",{})|value
            else:
                node[key] = copy.deepcopy(value)
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
    # Radio/platform hypotheses share the exogenous world; each run records them separately.
    result.pop("radio", None)
    for node in result["nodes"]:
        for name in ("candidate","interval_ms","duty_permille","listen_one_in","relay_records","relay_ttl_s", "phy", "tx_power_dbm"):
            node.pop(name,None)
    for action in result.get("actions",[]):
        if action["kind"]=="mode":
            action.pop("phy",None)
    return {"environment_seed":world.env_seed,"layout_seed":world.layout_seed,"world":result}


def assumptions(path):
    if path is None:
        return [{"name":"base","radio":{}}]
    data=load(path)
    if (not isinstance(data,dict) or set(data)!={"schema","cases"} or type(data["schema"]) is not int or
            data["schema"]!=1 or not isinstance(data["cases"],list) or not 1<=len(data["cases"])<=64):
        raise ValueError("assumptions require schema 1 and 1..64 named radio cases")
    names=set()
    for case in data["cases"]:
        if (not isinstance(case,dict) or set(case)!={"name","radio"} or
                not isinstance(case["name"],str) or not re.fullmatch(r"[A-Za-z0-9_.-]+",case["name"]) or
                len(case["name"])>64 or
                case["name"] in names or not isinstance(case["radio"],dict) or "seed" in case["radio"]):
            raise ValueError("each assumption needs a distinct safe name and radio settings, without a seed override")
        names.add(case["name"])
    return data["cases"]


def difference_summary(values):
    return {"n":len(values),"mean":statistics.mean(values) if values else None,
            "minimum":min(values) if values else None,"maximum":max(values) if values else None,
            "negative":sum(v<0 for v in values),"zero":sum(v==0 for v in values),
            "positive":sum(v>0 for v in values)}


def paired_results(runs, candidates, seeds):
    paired=[]
    for assumption,name in sorted({(r["assumption"],r["scenario"]) for r in runs}):
        for candidate in candidates[1:]:
            coverage,ages=[],[]
            for seed in seeds:
                row={r["candidate"]:r["summary"]["metrics"]["overall"] for r in runs
                     if r["scenario"]==name and r["assumption"]==assumption and r["seed"]==seed}
                a,b=row[candidates[0]],row[candidate]
                if a["coverage_fraction"] is not None and b["coverage_fraction"] is not None:
                    coverage.append(b["coverage_fraction"]-a["coverage_fraction"])
                if a["true_age"]["p95_ns"] is not None and b["true_age"]["p95_ns"] is not None:
                    ages.append((b["true_age"]["p95_ns"]-a["true_age"]["p95_ns"])/1e9)
            paired.append({"assumption":assumption,"scenario":name,"baseline":candidates[0],"candidate":candidate,
                           "worlds":len(seeds),"coverage_differences":coverage,"true_p95_differences_s":ages,
                           "coverage_summary":difference_summary(coverage),"true_p95_summary_s":difference_summary(ages)})
    return paired


def sensitivity_results(paired):
    results=[]
    for scenario,candidate in sorted({(r["scenario"],r["candidate"]) for r in paired}):
        rows=[r for r in paired if r["scenario"]==scenario and r["candidate"]==candidate]
        for metric in ("coverage_summary","true_p95_summary_s"):
            means={r["assumption"]:r[metric]["mean"] for r in rows if r[metric]["mean"] is not None}
            results.append({"scenario":scenario,"candidate":candidate,"baseline":rows[0]["baseline"],
                            "metric":metric,"mean_differences":means,
                            "sign_reversal":any(v<0 for v in means.values()) and any(v>0 for v in means.values())})
    return results


def git(*args):
    return subprocess.run(["git","-C",str(ROOT),*args],check=True,capture_output=True,text=True).stdout.strip()


def markdown(data):
    interval=data["experiment"]["interval_override_ms"]
    interval_note="none; declared setup/scenario cadence is used" if interval is None else f"{interval} ms; overrides every setup/scenario interval"
    lines=["# DES protocol comparison", "", "Evidence: actual C node and codec with a synthetic Python DES world. No physical-radio or phone claim.",
           "The relay/CAD candidates are research policies, not the proposed v4 format, sdk or Meshtastic firmware.", "",
           "## Candidate setups", "",
           "Labels identify the implementation and protocol settings below. Complete effective per-node settings and mode commands are saved in each scenario snapshot.",
           "Different settings can change offered RF load, airtime and feasible cadence. Such rows compare whole setups, not an isolated payload/access change; source trajectories, sensors and human actions remain paired.",
           f"Final CLI interval override: {interval_note}.", "",
           "| Label | Implementation | Declared settings |", "|---|---|---|"]
    for label,definition in data["candidate_setups"].items():
        lines.append(f"| {label} | {definition['implementation']} | `{json.dumps(definition['settings'],sort_keys=True)}` |")
    lines += ["", "## Results", "",
           "| Assumption | Scenario | Candidate | Seed | True age p95, s | Known pair-time | Position RMS, m | Busiest node RF, % | Violations |",
           "|---|---|---|---:|---:|---:|---:|---:|---:|"]
    for run in data["runs"]:
        summary=run["summary"]
        age=summary["metrics"]["overall"]["true_age"]["p95_ns"]
        coverage=summary["metrics"]["overall"]["coverage_fraction"]
        position=summary["metrics"]["overall"]["position_error"]["rms_m"]
        air=max((row["airtime_ns"] for row in summary["airtime"]),default=0)/summary["metrics"]["duration_ns"]*100
        lines.append(f"| {run['assumption']} | {run['scenario']} | {run['candidate']} | {run['seed']} | "
                     f"{'—' if age is None else f'{age/1e9:.4f}'} | {'—' if coverage is None else f'{coverage:.2%}'} | "
                     f"{'—' if position is None else f'{position:.3f}'} | {air:.3f} | {len(summary['contract_failures'])} |")
    lines += ["", "True ages are weighted by elapsed time. Coverage includes unknown pairs; a good conditional percentile alone does not establish group coverage.",
              "RF fraction is over this observation window; compliance checks use each node's actual rolling-hour cost and stated budget.",
              "Violations count only declared runtime checks: native source identity, transmission time availability and actual rolling-hour RF cost. Zero is not a pass of the entire architecture or hardware model.",
              "Native minute-bucket booked cost is reported separately. The decoder treats failed reception as erasure; bit corruption and recapture are unsupported.",
              "Reported clock ages use a piecewise affine calendar between observations, with the declared native tick-resolution error bound.", "",
              "Position RMS is conditional on having observed XY coordinates; unknown time remains separately reported in each summary.", "",
              "## Paired differences", "", "Each pair uses the same environment and layout realization. These are descriptive independent-run differences, not packet-level confidence intervals."]
    for row in data["paired"]:
        differences=row["coverage_differences"]
        age=row["true_p95_differences_s"]
        lines += [f"- {row['assumption']} / {row['scenario']}: {row['candidate']} minus {row['baseline']}, {row['worlds']} paired worlds; "
                  f"mean coverage difference {'—' if not differences else f'{statistics.mean(differences):+.2%}'}; "
                  f"mean known-age p95 difference {'—' if not age else f'{statistics.mean(age):+.4f} s'}. "
                  f"Age difference sign by world: {sum(v<0 for v in age)} negative, {sum(v==0 for v in age)} zero, {sum(v>0 for v in age)} positive. "
                  "Coverage and age must be assessed together."]
    if len(data["assumptions"])>1:
        lines += ["", "## Model sensitivity", "",
                  "Each named case changes only radio/platform assumptions; exogenous world hashes remain equal. "
                  "A sign reversal means the mean paired difference changes sign across these cases. "
                  "Absence of reversal only describes the selected cases, not all plausible models."]
        for row in data["sensitivity"]:
            values=", ".join(f"{name}={value:+.5g}" for name,value in row["mean_differences"].items()) or "unavailable"
            lines.append(f"- {row['scenario']}, {row['candidate']} − {row['baseline']}, {row['metric']}: {values}; "
                         f"sign reversal: {'yes' if row['sign_reversal'] else 'no'}.")
    lines += ["", "## Individual traces", ""]
    for run in data["runs"]:
        folder=run["directory"]
        lines.append(f"- {run['assumption']} / {run['scenario']} / {run['candidate']} / {run['seed']}: "
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
    parser.add_argument("--candidates",help="comma-separated setup labels and/or built-in implementations; default: setup entries, or v3,relay")
    parser.add_argument("--setups",type=Path,help="schema-1 named implementations and protocol-only settings")
    parser.add_argument("--assumptions",type=Path,help="JSON named radio-model cases, paired against one exogenous world")
    parser.add_argument("--seeds",type=numbers,default=[17,29,43])
    parser.add_argument("--shuffle-slots",action="store_true")
    parser.add_argument("--action-phase-s",type=float,default=0)
    parser.add_argument("--interval-ms",type=int)
    parser.add_argument("--reverse-ties",action="store_true")
    parser.add_argument("--reference",action="store_true",help="compute causal unlimited-content contact diagnostic")
    parser.add_argument("--no-replay",action="store_true")
    parser.add_argument("--output",type=Path)
    args=parser.parse_args(argv)
    configured=candidate_setups(args.setups)
    available={name:{"implementation":name,"settings":{}} for name in IMPLEMENTATIONS}|configured
    candidates=args.candidates.split(",") if args.candidates is not None else list(configured) if args.setups is not None else ["v3","relay"]
    if not candidates or len(set(candidates))!=len(candidates) or any(c not in available for c in candidates):
        parser.error("candidates must be distinct setup labels or built-in implementations")
    definitions={name:available[name] for name in candidates}
    if not 0<=args.action_phase_s<1e6:
        parser.error("action phase must be finite and nonnegative")
    if args.output is not None and args.output.exists() and (not args.output.is_dir() or any(args.output.iterdir())):
        parser.error("output must be new or empty; previous evidence is not overwritten")
    if sum((bool(args.scenario),args.suite,bool(args.generate)))>1:
        parser.error("choose --scenario, --suite or --generate")
    cases=assumptions(args.assumptions)
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
    source["candidate_setups_sha256"]=digest(definitions)
    (output/"candidate-setups.json").write_text(json.dumps({"schema":1,"candidates":definitions},indent=2,allow_nan=False)+"\n")
    runs=[]
    hashes={}
    for scenario in scenarios:
        for seed in args.seeds:
            env=int.from_bytes(keyed(seed,"environment")[:8],"big")
            layout=int.from_bytes(keyed(seed,"layout")[:8],"big")
            for case in cases:
                for candidate in candidates:
                    definition=definitions[candidate]
                    prepared=prepare(scenario,definition["implementation"],layout,args.shuffle_slots,
                                     args.action_phase_s,args.interval_ms,definition["settings"])
                    prepared["radio"]=prepared.get("radio",{})|case["radio"]
                    protocol=int.from_bytes(keyed(seed,"protocol",candidate)[:8],"big")
                    sim=Simulation(prepared,environment_seed=env,layout_seed=layout,
                                   protocol_seed=protocol,
                                   reverse_ties=args.reverse_ties,library=library)
                    try:
                        name=sim.world.config["name"]
                        prefix="" if args.assumptions is None else case["name"]+"-"
                        folder=output/f"{prefix}{name}-{candidate}-{seed}"
                        folder.mkdir()
                        world_hash=digest(world_manifest(sim.world))
                        pair=(name,seed)
                        if pair in hashes and hashes[pair]!=world_hash:
                            raise ValueError("paired candidates/models did not receive the same world")
                        hashes[pair]=world_hash
                        summary=sim.run(args.reference)
                        summary["run"]={"assumption":case["name"],"candidate":candidate,"seed":seed,
                                        "implementation":definition["implementation"],"settings":definition["settings"],
                                        "interval_override_ms":args.interval_ms,
                                        "environment_seed":env,"layout_seed":layout,
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
                        runs.append({"assumption":case["name"],"scenario":name,"candidate":candidate,"seed":seed,
                                     "implementation":definition["implementation"],"settings":definition["settings"],
                                     "directory":folder.name,"replay":not args.no_replay,"summary":summary})
                    finally:
                        sim.close()
    paired=paired_results(runs,candidates,args.seeds)
    data={"schema":1,"runs":runs,"paired":paired,"assumptions":cases,"candidate_setups":definitions,
          "experiment":{"interval_override_ms":args.interval_ms,
                        "comparison":"candidate setups on paired exogenous worlds; protocol settings may change offered RF load"},
          "sensitivity":sensitivity_results(paired),"source":source}
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
