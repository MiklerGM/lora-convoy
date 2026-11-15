"""Named receiver assumptions must compare the same exogenous realizations."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from .native import ROOT


class ComparisonTest(unittest.TestCase):
    def test_named_setups_apply_real_phy_cadence_and_keep_labels_separate_from_implementations(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            relay_label="relay-long-"+"x"*21
            scene={"schema":1,"name":"s"*128,"duration_s":18,"warmup_s":2,
                   "default_link":{"decode":True,"sense":True,"interfere":True,"power_dbm":-90},
                   "nodes":[{"id":f"n{i}","group":"g","slot":i,"interval_ms":1000,
                             "phy":{"sync_word":0x1234},"trajectory":[{"t_s":0,"x_m":i*10,"y_m":0}]}
                            for i in range(3)],
                   "actions":[{"id":f"mode-{i}","kind":"mode","node":f"n{i}","t_s":17.5,"phy":{"sf":9}}
                              for i in range(3)]}
            definitions={"v3":{"implementation":"v3","settings":{"interval_ms":4000,"listen_one_in":0}},
                         relay_label:{"implementation":"relay","settings":{"interval_ms":6000,"listen_one_in":0,
                             "phy":{"sf":8,"bandwidth_hz":125000,"frequency_hz":869475000}}}}
            (root/"scene.json").write_text(json.dumps(scene))
            (root/"setups.json").write_text(json.dumps({"schema":1,"candidates":definitions}))
            (root/"cases.json").write_text(json.dumps({"schema":1,"cases":[{"name":"a"*64,"radio":{}}]}))
            command=[sys.executable,str(ROOT/"tools/simulate.py"),"--scenario",str(root/"scene.json"),
                     "--setups",str(root/"setups.json"),"--no-replay"]
            result=subprocess.run(command+["--assumptions",str(root/"cases.json"),"--seeds",str(2**64-1),
                                           "--output",str(root/"out")],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            data=json.loads((root/"out/results.json").read_text())
            self.assertEqual(data["candidate_setups"],definitions)
            self.assertEqual({run["candidate"] for run in data["runs"]},set(definitions))
            self.assertEqual(len({r["summary"]["run"]["world_sha256"] for r in data["runs"]}),1)
            actual={}
            for run in data["runs"]:
                definition=definitions[run["candidate"]]
                metadata=run["summary"]["run"]
                self.assertEqual(metadata["implementation"],definition["implementation"])
                self.assertEqual(metadata["settings"],definition["settings"])
                self.assertIsNone(metadata["interval_override_ms"])
                self.assertIn(metadata["implementation"],run["summary"]["candidate_capabilities"])
                folder=root/"out"/run["directory"]
                self.assertLessEqual(len(folder.name.encode()),255)
                effective=json.loads((folder/"scenario.json").read_text())
                self.assertTrue(all(n["candidate"]==definition["implementation"] for n in effective["nodes"]))
                self.assertTrue(all(n["interval_ms"]==definition["settings"]["interval_ms"] for n in effective["nodes"]))
                self.assertTrue(all(n["phy"]["sync_word"]==0x1234 for n in effective["nodes"]))
                trace=[json.loads(line) for line in (folder/"events.jsonl").read_text().splitlines()]
                actual[run["candidate"]]=[e for e in trace if e["event"]=="tx" and e["t_ns"]<17500000000]
            self.assertEqual({e["sender"] for e in actual["v3"]},{"n0","n1","n2"})
            self.assertEqual(sum(e["sender"]=="n0" for e in actual["v3"]),4)
            self.assertTrue(all(e["bytes"]==25 and e["end_ns"]-e["start_ns"]==30848000 for e in actual["v3"]))
            relay=actual[relay_label]
            self.assertTrue(relay)
            self.assertEqual(sum(e["sender"]=="n0" for e in relay),2)
            self.assertTrue(all(e["phy"]["sf"]==8 and e["phy"]["bandwidth_hz"]==125000 and
                                e["phy"]["crc"] and e["phy"]["frequency_hz"]==869475000 for e in relay))
            # Independent LoRa airtime vectors, with a CRC: 25B -> 55.25 symbols; 43B (two riders
            # carried) -> 80.25, at 2048us/symbol.
            self.assertEqual(relay[0]["bytes"],25)
            self.assertEqual(relay[0]["end_ns"]-relay[0]["start_ns"],113152000)
            self.assertTrue(any(e["bytes"]==43 and e["end_ns"]-e["start_ns"]==164352000 for e in relay))
            self.assertEqual(max(len(r["directory"].encode()) for r in data["runs"]),247)
            report=(root/"out/report.md").read_text()
            self.assertIn("Different settings can change offered RF load",report)
            self.assertIn(relay_label,report)

            override=subprocess.run(command+["--candidates",relay_label+",v3-cad","--seeds","17",
                "--interval-ms","2000","--output",str(root/"override")],capture_output=True,text=True)
            self.assertEqual(override.returncode,0,override.stderr)
            changed=json.loads((root/"override/results.json").read_text())
            self.assertEqual({r["candidate"] for r in changed["runs"]},{relay_label,"v3-cad"})
            self.assertEqual(changed["candidate_setups"][relay_label]["settings"]["interval_ms"],6000)
            self.assertEqual(changed["experiment"]["interval_override_ms"],2000)
            for run in changed["runs"]:
                self.assertEqual(run["summary"]["run"]["interval_override_ms"],2000)
                folder=root/"override"/run["directory"]
                effective=json.loads((folder/"scenario.json").read_text())
                self.assertTrue(all(n["interval_ms"]==2000 for n in effective["nodes"]))
                if run["candidate"]==relay_label:
                    trace=[json.loads(line) for line in (folder/"events.jsonl").read_text().splitlines()]
                    self.assertEqual(sum(e["event"]=="tx" and e["sender"]=="n0" and e["t_ns"]<17500000000
                                         for e in trace),8)

            invalid=[{"schema":1,"candidates":{"bad":{"implementation":"relay","settings":settings}}}
                     for settings in ({"gnss":{}},{"interval_ms":True},{"phy":{"sf":True}},{"phy":{"rssi":-90}})]
            invalid += [{"schema":1,"candidates":{"v3":{"implementation":"relay","settings":{}}}},
                        {"schema":1,"candidates":{"x"*33:{"implementation":"v3","settings":{}}}}]
            for item in [*(json.dumps(case) for case in invalid),
                         '{"schema":1,"candidates":{"dup":{"implementation":"v3","settings":{}},'
                         '"dup":{"implementation":"relay","settings":{}}}}']:
                with self.subTest(invalid=item):
                    (root/"bad.json").write_text(item)
                    rejected=subprocess.run([sys.executable,str(ROOT/"tools/simulate.py"),"--setups",str(root/"bad.json"),
                        "--seeds","17","--output",str(root/"bad-output")],capture_output=True,text=True)
                    self.assertNotEqual(rejected.returncode,0)
                    self.assertFalse((root/"bad-output").exists())

    def test_named_models_preserve_worlds_and_expose_conditional_reversals(self):
        # A lossy receiver-model change must not redraw the environment or mix paired rows.
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            scene={"schema":1,"name":"local-noise","duration_s":12,"warmup_s":2,
                   "default_link":{"decode":True,"sense":True,"interfere":True,"power_dbm":-90},
                   "nodes":[{"id":f"n{i}","group":"g","slot":i,"interval_ms":1000,"listen_one_in":0,
                             "trajectory":[{"t_s":0,"x_m":i*10,"y_m":0}]} for i in range(2)],
                   "noise":{"background_dbm":-120,
                            "bursts":[{"from_s":1,"until_s":12,"receivers":["n1"],"power_dbm":-60}]}}
            (root/"scene.json").write_text(json.dumps(scene))
            cases={"schema":1,"cases":[{"name":"ignore-noise","radio":{}},
                                       {"name":"erase-noise","radio":{"noise_threshold_dbm":-100}}]}
            (root/"models.json").write_text(json.dumps(cases))
            result=subprocess.run([sys.executable,str(ROOT/"tools/simulate.py"),
                                   "--scenario",str(root/"scene.json"),"--assumptions",str(root/"models.json"),
                                   "--seeds","17,29","--no-replay","--output",str(root/"out")],
                                  text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            data=json.loads((root/"out/results.json").read_text())
            self.assertEqual(len(data["runs"]),8)
            for seed in (17,29):
                runs=[r for r in data["runs"] if r["seed"]==seed]
                self.assertEqual(len({r["summary"]["run"]["world_sha256"] for r in runs}),1)
                direct={r["assumption"]:r["summary"]["metrics"]["overall"]["coverage_fraction"]
                        for r in runs if r["candidate"]=="v3"}
                self.assertEqual(direct["ignore-noise"],1)
                self.assertEqual(direct["erase-noise"],.5)
            self.assertEqual({r["assumption"] for r in data["paired"]},{"ignore-noise","erase-noise"})
            self.assertTrue(all(r["worlds"]==2 and r["coverage_summary"]["n"]==2 for r in data["paired"]))
            sys.path.insert(0,str(ROOT/"tools"))
            try:
                from simulate import sensitivity_results
                rows=[{"scenario":"chain","candidate":"relay","baseline":"v3","assumption":name,
                       "coverage_summary":{"mean":coverage},"true_p95_summary_s":{"mean":age}}
                      for name,coverage,age in (("a",0,-1),("b",0,2),("c",None,None))]
                sensitivity=sensitivity_results(rows)
                self.assertFalse(sensitivity[0]["sign_reversal"])
                self.assertTrue(sensitivity[1]["sign_reversal"])
                self.assertEqual(sensitivity[1]["mean_differences"],{"a":-1,"b":2})
            finally:
                sys.path.pop(0)


if __name__=="__main__":
    unittest.main()
