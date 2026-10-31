"""Cross the actual C codec/core boundary and the executable DES composition."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from .native import NativeLibrary, NativeNode, ROOT
from .runtime import Simulation
from .types import NS, Phy


def chain(candidate):
    return {"schema":1,"name":"causal-chain","duration_s":48,"warmup_s":20,
            "nodes":[{"id":f"n{i}","group":"g","slot":i,"candidate":candidate,
                      "interval_ms":4000,"listen_one_in":0,
                      "trajectory":[{"t_s":0,"x_m":i*100,"y_m":0},
                                    {"t_s":48,"x_m":i*100+480,"y_m":0}]}
                     for i in range(5)],
            "links":[{"from":f"n{i}","to":f"n{j}"} for i in range(5) for j in range(5) if abs(i-j)==1],
            "actions":[{"id":"on","node":"n0","t_s":22,"kind":"state","state":"help"},
                       {"id":"off","node":"n0","t_s":34,"kind":"state","state":"normal"},
                       {"id":"restart","node":"n4","t_s":30,"kind":"reboot"}]}


class RuntimeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library=NativeLibrary()

    def test_native_authentication_and_sensor_bounds_use_the_actual_c(self):
        for size,options,expected in (
                (25,{},30848000),(25,{"crc":False},30848000),
                (25,{"implicit_header":True},28288000),
                (60,{"sf":8,"bandwidth_hz":125000,"crc":False},195072000),
                (60,{"sf":8,"bandwidth_hz":125000},205312000),
                (25,{"sf":12,"bandwidth_hz":125000},1482752000)):
            with self.subTest(phy=options):
                self.assertEqual(Phy(**options).airtime_ns(size),expected)
        cfg={"slot":0,"candidate":"v3","interval_ms":4000,"listen_one_in":0,
             "gnss":{"nmea_told_lag_ms":0,"nmea_error_ms":100},"phy":vars(Phy())}
        sender=NativeNode(self.library,cfg,2,b"k"*16,17)
        receiver=NativeNode(self.library,cfg|{"slot":1},2,b"k"*16,29)
        try:
            for node in (sender,receiver):
                node.pps(1000000)
                node.fix(1100000,1792670400000,True,True,471410000,95215000,100,0)
            due=sender.next(1100000)
            packet=sender.poll(due)
            self.assertEqual(len(packet),25)
            at=due+Phy().airtime_ns(25)//1000
            damaged=bytearray(packet)
            damaged[12]^=1
            self.assertEqual(receiver.receive(bytes(damaged),at,stamp_us=at),[])
            self.assertIsNone(receiver.view(0))
            self.assertEqual(receiver.receive(packet,at,stamp_us=at),[0])
            self.assertEqual(receiver.view(0)["counter"],sender.records(packet)[0]["counter"])
            self.assertEqual(receiver.receive(packet,at+1000,stamp_us=at+1000),[])
            with self.assertRaisesRegex(ValueError,"C GNSS interface"):
                sender.fix(1200000,1792670400000,True,True,471410000,95215000,10**20,0)
            relay=NativeNode(self.library,cfg|{"candidate":"relay"},2,b"k"*16,17)
            try:
                relay.pps(1000000)
                relay.fix(1100000,1792670400000,True,True,471410000,95215000,100,0)
                changed=Phy(implicit_header=True)
                relay.mode(changed,False)
                pending=relay.next(1100000)
                self.assertEqual(relay.poll(pending),b"")
                self.assertEqual(relay.statistics(pending)["core_duty_used_ms"],0)
                relay.mode(changed,True)
                emitted=relay.next(pending)
                packet=relay.poll(emitted)
                self.assertEqual(len(packet),29)
                self.assertEqual(relay.records(packet)[0]["counter"],0)
                foreign_due=receiver.next(emitted+1000)
                foreign=receiver.poll(foreign_due)
                received=foreign_due+Phy().airtime_ns(25)//1000
                self.assertEqual(relay.receive(foreign,received,stamp_us=received),[1])
                before=relay.statistics(received)["core_duty_used_ms"]
                forwarding_due=relay.next(received)
                forwarded=relay.poll(forwarding_due)
                self.assertEqual(len(forwarded),54)
                full_cost=(changed.airtime_ns(54)+999999)//1000000
                self.assertEqual(relay.statistics(forwarding_due)["core_duty_used_ms"]-before,full_cost)
                self.assertEqual(len(relay.poll(relay.next(received+31000000))),29)
            finally:
                relay.close()
        finally:
            sender.close()
            receiver.close()

    def test_relay_preserves_origins_and_receiver_reboot_forgets_views(self):
        results={}
        for candidate in ("v3","relay"):
            sim=Simulation(chain(candidate),library=self.library)
            try:
                summary=sim.run()
                results[candidate]=(summary,copy.deepcopy(sim.events))
                self.assertEqual(summary["contract_failures"],[])
                self.assertEqual(summary["metrics"]["outside_group_applications"],0)
            finally:
                sim.close()
        direct=next(p for p in results["v3"][0]["metrics"]["pairs"] if p["receiver"]=="n4" and p["author"]=="n0")
        relay=next(p for p in results["relay"][0]["metrics"]["pairs"] if p["receiver"]=="n4" and p["author"]=="n0")
        self.assertEqual(direct["view_ns"],0)
        self.assertIsNotNone(relay["final_view"])
        records={e["record"]["key"]:e["record"] for e in results["relay"][1] if e["event"]=="source_record"}
        heard=[e for e in results["relay"][1] if e["event"]=="applied" and e["receiver"]=="n4" and e["author"]=="n0"]
        self.assertTrue(heard)
        self.assertTrue(all(e["via"]=="n3" for e in heard))
        self.assertTrue(all(records[e["record_key"]]["author"]=="n0" for e in heard))
        self.assertTrue(any(e["event"]=="view_reset" and e["receiver"]=="n4" and e["t_ns"]==30*NS
                            for e in results["relay"][1]))
        reset=Simulation(chain("relay"),library=self.library)
        try:
            reset.engine.run(30*NS)
            self.assertTrue(all(pair.view is None for (receiver,_),pair in reset.evaluator.pairs.items() if receiver=="n4"))
        finally:
            reset.close()
        role={"schema":1,"name":"forgotten-fork","duration_s":32,"warmup_s":5,
              "nodes":[{"id":node,"group":"g","slot":slot,"listen_one_in":0,
                        "trajectory":[{"t_s":0,"x_m":slot*100,"y_m":0}]}
                       for slot,node in enumerate(("a","b"))],
              "links":[{"from":"b","to":"a","until_s":29}],
              "actions":[{"id":"restart","node":"a","kind":"reboot","t_s":30}],
              "decisions":[{"id":"before","node":"a","kind":"fork","at_s":28,"after_s":10,"watch":["b"]},
                           {"id":"after","node":"a","kind":"fork","at_s":31,"after_s":10,"watch":["b"]}]}
        observer=Simulation(role,library=self.library)
        try:
            summary=observer.run()
            self.assertTrue(summary["role_decisions"][0]["information_available"])
            self.assertFalse(summary["role_decisions"][1]["information_available"])
        finally:
            observer.close()
        clock_world={"schema":1,"name":"pps-cadence","duration_s":12,"warmup_s":2,
                     "default_link":{"decode":True,"sense":True,"interfere":True},
                     "nodes":[{"id":f"n{i}","group":"g","slot":i,"interval_ms":1000,
                               "listen_one_in":0,"gnss":{"period_s":.1,"lag_ms":100},
                               "trajectory":[{"t_s":0,"x_m":i*100,"y_m":0}]} for i in range(2)]}
        for period,lag in ((.1,100),(1,1000)):
            signatures=[]
            for reverse in (False,True):
                scene=copy.deepcopy(clock_world)
                for node in scene["nodes"]:
                    node["gnss"]={"period_s":period,"lag_ms":lag}
                clock=Simulation(scene,library=self.library,reverse_ties=reverse)
                try:
                    clock.run()
                    signatures.append(sorted((e["node"],e["t_ns"],e["true_error_ns"])
                                             for e in clock.events if e["event"]=="tx_clock"))
                finally:
                    clock.close()
            self.assertTrue(signatures[0])
            self.assertEqual(signatures[0],signatures[1])

    def test_cli_pairs_worlds_and_preserves_reproducible_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            output=Path(temporary)/"comparison"
            command=[sys.executable,str(ROOT/"tools/simulate.py"),"--generate","chain","--size","3",
                     "--seeds","17","--shuffle-slots","--reference","--output",str(output)]
            subprocess.run(command,check=True,capture_output=True,text=True)
            original=(output/"results.json").read_bytes()
            data=json.loads(original)
            self.assertEqual(len(data["runs"]),2)
            self.assertEqual({r["candidate"] for r in data["runs"]},{"v3","relay"})
            hashes={r["summary"]["run"]["world_sha256"] for r in data["runs"]}
            self.assertEqual(len(hashes),1)
            for run in data["runs"]:
                folder=output/run["directory"]
                self.assertEqual(json.loads((folder/"scenario.json").read_text())["nodes"][0]["candidate"],run["candidate"])
                trace=[json.loads(line) for line in (folder/"events.jsonl").read_text().splitlines()]
                self.assertEqual(trace[0]["world_sha256"],run["summary"]["run"]["world_sha256"])
                self.assertTrue(any(e["event"]=="tx" for e in trace))
                self.assertTrue((folder/"replay.html").is_file())
            second=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(second.returncode,2)
            self.assertEqual((output/"results.json").read_bytes(),original)
            singleton=Path(temporary)/"singleton.json"
            singleton.write_text(json.dumps({"schema":1,"name":"single","duration_s":4,
                "nodes":[{"id":"alone","group":"noise","slot":0,"interval_ms":1000,
                          "trajectory":[{"t_s":0,"x_m":0,"y_m":0}]}]}))
            isolated=Path(temporary)/"isolated"
            subprocess.run([sys.executable,str(ROOT/"tools/simulate.py"),"--scenario",str(singleton),
                            "--seeds","17","--no-replay","--output",str(isolated)],
                           check=True,capture_output=True,text=True)
            zero=json.loads((isolated/"results.json").read_text())
            self.assertTrue(all(r["summary"]["metrics"]["overall"]["pair_count"]==0 for r in zero["runs"]))
            self.assertEqual(zero["paired"][0]["coverage_differences"],[])


if __name__=="__main__":
    unittest.main()
