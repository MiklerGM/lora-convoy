"""Named receiver assumptions must compare the same exogenous realizations."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from .native import ROOT


class ComparisonTest(unittest.TestCase):
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
