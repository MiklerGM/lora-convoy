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
            self.assertEqual(receiver.view(0)["as_of"],sender.records(packet)[0]["as_of"])
            self.assertEqual(receiver.view(0)["via"],0)
            self.assertEqual(receiver.receive(packet,at+1000,stamp_us=at+1000),[])
            with self.assertRaisesRegex(ValueError,"C GNSS interface"):
                sender.fix(1200000,1792670400000,True,True,471410000,95215000,10**20,0)
            relay=NativeNode(self.library,cfg|{"candidate":"relay","relay_records":1},2,b"k"*16,17)
            try:
                relay.pps(1000000)
                relay.fix(1100000,1792670400000,True,True,471410000,95215000,100,0)
                # the core sends with an explicit header and a CRC only, as a board does
                with self.assertRaisesRegex(ValueError,"implicit/no-CRC"):
                    relay.mode(Phy(implicit_header=True),False)
                changed=Phy(sf=8)
                relay.mode(changed,False)
                pending=relay.next(1100000)
                self.assertEqual(relay.poll(pending),b"")
                self.assertEqual(relay.statistics(pending)["core_duty_used_ms"],0)
                relay.mode(changed,True)
                emitted=relay.next(pending)
                packet=relay.poll(emitted)
                # nobody heard yet, nobody to carry: its own record alone
                self.assertEqual(len(packet),25)
                self.assertEqual(len(relay.records(packet)),1)
                foreign_due=receiver.next(emitted+1000)
                foreign=receiver.poll(foreign_due)
                received=foreign_due+Phy().airtime_ns(25)//1000
                self.assertEqual(relay.receive(foreign,received,stamp_us=received),[1])
                before=relay.statistics(received)["core_duty_used_ms"]
                carrying_due=relay.next(received)
                carrying=relay.poll(carrying_due)
                # the rider in slot 1 carried along, as it said itself, and booked at the full length
                self.assertEqual(len(carrying),34)
                own,passenger=relay.records(carrying)
                self.assertEqual((own["slot"],passenger["slot"],passenger["via"]),(0,1,0))
                self.assertEqual(passenger["as_of"],receiver.records(foreign)[0]["as_of"])
                self.assertEqual(relay.statistics(carrying_due)["core_duty_used_ms"]-before,
                                 (changed.airtime_ns(34)+999999)//1000000)
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
              "origin":{"lat_deg":35,"lon_deg":139},
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
            a_position=next(e["record"]["position"] for e in observer.events
                            if e["event"]=="source_record" and e["record"]["author"]=="a")
            self.assertLess(abs(a_position[0]),3)
            self.assertLess(abs(a_position[1]),3)
            own_tx=next(e for e in observer.events if e["event"]=="tx" and e["sender"]=="a")
            own_wire=observer.nodes["a"].records(bytes.fromhex(own_tx["payload_hex"]))[0]
            # Half a 24-bit coordinate step: 90/2^23 latitude, 180/2^23 longitude, in e7 units.
            self.assertAlmostEqual(own_wire["lat_e7"],350000000,delta=55)
            self.assertAlmostEqual(own_wire["lon_e7"],1390000000,delta=109)
            self.assertEqual(summary["numerical_assumptions"]["geographic_projection"]["origin"],role["origin"])
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

    def test_cad_defers_for_local_noise_and_stays_inside_symmetric_guard(self):
        scene={"schema":1,"name":"cad-local","duration_s":6,"warmup_s":0,
               "radio":{"cad_noise_threshold_dbm":-100,"noise_threshold_dbm":-100,
                        "irq_delay_ns":300000,"tx_prepare_ns":2000000},
               "default_link":{"decode":True,"sense":True,"interfere":True,"power_dbm":-90},
               "nodes":[{"id":node,"group":"g","slot":slot,"candidate":"v3-cad",
                         "interval_ms":1000,"listen_one_in":0,
                         "clock":{"drift_ppm":20 if slot==0 else -20},
                         "gnss":{"nmea_only":bool(slot),"nmea_told_lag_ms":100,"nmea_error_ms":120},
                         "trajectory":[{"t_s":0,"x_m":slot*100,"y_m":0}]}
                        for slot,node in enumerate(("a","b"))]}
        probe=Simulation(scene,library=self.library)
        try:
            probe.engine.run(NS//5)
            due=probe.nodes["a"].next(probe.world.local_us("a",NS//5))
            nominal=probe.world.world_deadline("a",due)
        finally:
            probe.close()
        scene["noise"]={"bursts":[{"from_s":(nominal-1000000)/NS,
                                     "until_s":(nominal+2000000)/NS,
                                     "receivers":["a"],"power_dbm":-60}]}
        simulation=Simulation(scene,library=self.library)
        try:
            summary=simulation.run()
            cad=[e for e in simulation.events if e["event"]=="cad" and e["node"]=="a"]
            self.assertTrue(cad[0]["busy"])
            self.assertFalse(cad[1]["busy"])
            tx=[e for e in simulation.events if e["event"]=="tx" and e["sender"]=="a"]
            self.assertGreater(tx[0]["start_ns"],nominal+2000000)
            self.assertGreaterEqual(tx[0]["start_ns"],cad[1]["t_ns"]+2300000)
            for event in (e for e in simulation.events if e["event"]=="tx_clock"):
                self.assertLessEqual(event["start_offset_ns"]+event["declared_error_ns"]+1000000,
                                     (NS//3-30848000-2000000)//2)
            all_tx=[e for e in simulation.events if e["event"]=="tx"]
            for index,left in enumerate(all_tx):
                for right in all_tx[index+1:]:
                    if left["sender"]!=right["sender"]:
                        self.assertFalse(left["start_ns"]<right["end_ns"] and right["start_ns"]<left["end_ns"])
            cost=next(row for row in summary["airtime"] if row["node"]=="a")
            self.assertEqual(cost["access_cad_busy"],1)
            self.assertGreater(cost["access_released"],0)
            self.assertEqual(summary["contract_failures"],[])
        finally:
            simulation.close()

    def test_cad_does_not_see_hidden_transmissions_or_release_expired_or_cancelled_packets(self):
        hidden=json.loads((ROOT/"tools/simulation/hidden-terminals.json").read_text())
        for node in hidden["nodes"]:
            node.update(candidate="v3-cad",listen_one_in=0)
        simulation=Simulation(hidden,library=self.library)
        try:
            simulation.run()
            cad=[e for e in simulation.events if e["event"]=="cad" and e["node"] in ("a","c")]
            self.assertTrue(cad)
            self.assertTrue(all(not event["busy"] for event in cad))
            self.assertTrue(any(e["event"]=="rx" and not e["accepted"] and "interference" in e["reason"]
                                and e["receiver"] in ("b","d") for e in simulation.events))
        finally:
            simulation.close()
        cfg={"slot":0,"candidate":"v3-cad","interval_ms":1000,"listen_one_in":0,
             "gnss":{"nmea_told_lag_ms":0,"nmea_error_ms":100},"phy":vars(Phy())}
        for outcome in ("expiry","mode","exhausted"):
            with self.subTest(outcome=outcome):
                node=NativeNode(self.library,cfg,2,b"k"*16,17)
                try:
                    node.pps(1000000)
                    node.fix(1100000,1792670400000,True,True,471410000,95215000,100,0)
                    due=node.next(1100000)
                    action,created,payload=node.access_poll(due)
                    self.assertEqual((action,created,len(payload)),(1,True,25))
                    if outcome=="mode":
                        node.mode(Phy(sf=8),False)
                    if outcome=="exhausted":
                        attempt_at=due
                        for attempt in range(3):
                            node.cad_result(attempt_at+1024,False)
                            if attempt<2:
                                attempt_at=node.next(attempt_at+1024)
                                self.assertEqual(node.access_poll(attempt_at),(1,False,b""))
                        result_at=attempt_at+21024
                    else:
                        result_at=due+(1024 if outcome=="mode" else 200000)
                        node.cad_result(result_at,True)
                    self.assertEqual(node.access_poll(result_at),(0,False,b""))
                    stats=node.statistics(result_at)
                    key={"mode":"access_cancelled","expiry":"access_expired","exhausted":"access_exhausted"}[outcome]
                    self.assertEqual(stats[key],1)
                    if outcome=="exhausted":
                        self.assertEqual(stats["access_cad_requests"],3)
                    self.assertEqual(stats["access_released"],0)
                    self.assertEqual(stats["access_booked_ms"],31)
                    self.assertEqual(stats["access_dropped_booked_ms"],31)
                finally:
                    node.close()

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
