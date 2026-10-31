"""Observable radio contracts, using the real engine and scripted world."""
from dataclasses import replace
import unittest

from tools.simulator.kernel import Engine, WORLD, DECIDE
from tools.simulator.radio import RadioNetwork
from tools.simulator.types import Phy, NS
from tools.simulator.world import World


def nodes(names):
    return [{"id": name, "group": "g", "slot": slot,
             "trajectory": [{"t_s": 0, "x_m": slot, "y_m": 0}]}
            for slot, name in enumerate(names)]


class RadioTest(unittest.TestCase):
    def network(self, scene, settings=None, reverse=False):
        if "default_link" in scene:
            scene = {**scene, "default_link": {"decode": True, "sense": True,
                                               "interfere": True, **scene["default_link"]}}
        world = World({"schema": 1, "name": "radio-test", "duration_s": .5,
                       "nodes": nodes("abc"), **scene})
        engine = Engine(reverse_ties=reverse)
        observations, events = [], []
        network = RadioNetwork(engine, world,
                               lambda node, event: observations.append((node, event, engine.now_ns)),
                               events.append, settings)
        for node in world.node_ids:
            network.add_node(node, world.phy(node))
        for boundary in world.boundaries():
            engine.schedule(boundary, WORLD, network.environment_changed)
        return engine, network, observations, events

    def test_receiver_local_noise_inside_packet_and_at_its_end(self):
        # Independently known explicit-header/CRC airtime of 25 bytes at SF7/250, preamble 8.
        end = 30_848_000
        for start, stop, b_succeeds in ((15_000_000, 25_000_000, False),
                                        (end, end + 10_000_000, True)):
            with self.subTest(start=start):
                engine, radio, observed, trace = self.network({
                    "default_link": {"power_dbm": -80},
                    "noise": {"background_dbm": -120, "bursts": [
                        {"from_s": start / NS, "until_s": stop / NS,
                         "receivers": ["b"], "power_dbm": -50}]}},
                    {"noise_threshold_dbm": -100, "trace_intervals": True})
                engine.schedule(0, DECIDE, radio.transmit, "a", b"x" * 25)
                engine.run(100_000_000)
                tx = next(event for event in trace if event["event"] == "tx")
                self.assertEqual(tx["end_ns"], end)
                self.assertEqual([o.at_ns for n, o, _ in observed if n == "a" and o.kind == "TX_DONE"], [end])
                received = {n for n, o, _ in observed if o.kind == "RX_DONE"}
                self.assertEqual(received, {"b", "c"} if b_succeeds else {"c"})
                if not b_succeeds:
                    failure = next(event for event in trace if event["event"] == "rx" and event["receiver"] == "b")
                    self.assertEqual(failure["reason"], "external_noise")
                    affected = [event for event in trace if event["event"] == "rx_interval" and
                                event["receiver"] == "b" and event["noise_dbm"] > -100]
                    self.assertEqual([(i["start_ns"], i["end_ns"]) for i in affected], [(start, stop)])

    def test_simultaneous_cad_has_no_free_reservation_and_sensing_is_local(self):
        results = []
        for reverse in (False, True):
            engine, radio, observed, trace = self.network({"default_link": {"power_dbm": -80}}, reverse=reverse)
            def observation(node, event):
                observed.append((node, event, engine.now_ns))
                if event.kind == "CAD_DONE" and event.success:
                    engine.schedule(engine.now_ns, DECIDE, radio.transmit, node, node.encode() * 25)
            radio.observe = observation
            engine.schedule(0, DECIDE, radio.cad, "a", 1_000_000)
            engine.schedule(0, DECIDE, radio.cad, "b", 1_000_000)
            engine.run(100_000_000)
            clear = sorted(n for n, o, _ in observed if o.kind == "CAD_DONE" and o.success)
            sent = sorted((e["sender"], e["start_ns"]) for e in trace if e["event"] == "tx")
            self.assertEqual(clear, ["a", "b"])
            self.assertEqual(sent, [("a", 1_000_000), ("b", 1_000_000)])
            self.assertEqual([o for n, o, _ in observed if n == "c" and o.kind == "RX_DONE"], [])
            results.append((clear, sent))
        self.assertEqual(results[0], results[1])
        for sensing in (False, True):
            with self.subTest(sensing=sensing):
                engine, radio, observed, _ = self.network({"links": [
                    {"from": "a", "to": "b", "decode": False,
                     "sense": sensing, "interfere": True, "power_dbm": -80}]})
                engine.schedule(0, DECIDE, radio.cad, "b", 10_000_000)
                engine.schedule(0, DECIDE, radio.transmit, "a", b"x" * 25)
                engine.run(50_000_000)
                decisions = [o.success for n, o, _ in observed if n == "b" and o.kind == "CAD_DONE"]
                self.assertEqual(decisions, [not sensing])

    def test_settings_reboot_and_combined_power_govern_actual_reception(self):
        new_phy = replace(Phy(), sf=9)
        engine, radio, observed, trace = self.network({"nodes": nodes("ab"),
                                                     "default_link": {"power_dbm": -80}},
                                                    {"irq_delay_ns": 100_000_000})
        radio.sleep("b")
        engine.schedule(0, DECIDE, radio.transmit, "a", b"first" * 5)
        engine.schedule(10_000_000, DECIDE, radio.configure, "a", new_phy)
        engine.schedule(20_000_000, DECIDE, radio.listen, "b")
        engine.schedule(31_000_000, DECIDE, radio.configure, "b", new_phy)
        engine.schedule(40_000_000, DECIDE, radio.transmit, "a", b"abort" * 5)
        engine.schedule(60_000_000, WORLD, radio.reboot, "a")
        engine.schedule(100_000_000, DECIDE, radio.configure, "a", new_phy)
        engine.schedule(110_000_000, DECIDE, radio.transmit, "a", b"third" * 5)
        engine.run(400_000_000)
        sent = [event for event in trace if event["event"] == "tx"]
        self.assertEqual([e["phy"]["sf"] for e in sent], [7, 9, 9])
        self.assertEqual(sent[0]["end_ns"], 30_848_000)
        self.assertEqual([o.payload for n, o, _ in observed if n == "b" and o.kind == "RX_DONE"], [b"third" * 5])
        # First TX's queued IRQ is stale after reboot; the aborted TX never earns TxDone.
        self.assertEqual([o.at_ns for n, o, _ in observed if n == "a" and o.kind == "TX_DONE"], [sent[2]["end_ns"]])
        aborted = [e for e in trace if e["event"] == "tx_end" and e["aborted"]]
        self.assertEqual([(e["t_ns"], e["airtime_ns"]) for e in aborted], [(60_000_000, 20_000_000)])
        engine, radio, observed, _ = self.network({"nodes": nodes("ab"),
                                                  "default_link": {"power_dbm": -80}},
                                                 {"irq_delay_ns": 100_000_000})
        engine.schedule(0, DECIDE, radio.transmit, "a", b"x" * 25)
        engine.schedule(10_000_000, WORLD, radio.reboot, "b")
        engine.run(200_000_000)
        self.assertEqual([(n, o.kind) for n, o, _ in observed if n == "b"], [])
        engine, radio, observed, _ = self.network({"nodes": nodes("ab"),
                                                  "default_link": {"power_dbm": -80}},
                                                 {"irq_delay_ns": 100_000_000})
        engine.schedule(0, DECIDE, radio.transmit, "a", b"x" * 25)
        engine.schedule(10_000_000, DECIDE, radio.configure, "a", new_phy)
        engine.schedule(10_000_000, DECIDE, radio.configure, "b", new_phy)
        engine.run(200_000_000)
        self.assertEqual([o.kind for n, o, _ in observed if n == "b"], ["MODE_APPLIED"])
        self.assertEqual([o.at_ns for n, o, _ in observed if n == "a" and o.kind == "TX_DONE"], [30_848_000])
        engine, radio, observed, _ = self.network({"nodes": nodes("ab")}, {"irq_delay_ns": 100_000_000})
        engine.schedule(0, DECIDE, radio.cad, "b", 1_000_000)
        engine.schedule(10_000_000, DECIDE, radio.configure, "b", new_phy)
        engine.run(200_000_000)
        self.assertEqual([o.kind for n, o, _ in observed if n == "b"], ["MODE_APPLIED"])
        engine, radio, observed, _ = self.network({"nodes": nodes("ab")})
        engine.schedule(0, DECIDE, radio.configure, "b", new_phy, 50_000_000)
        engine.schedule(10_000_000, DECIDE, radio.sleep, "b")
        engine.run(100_000_000)
        self.assertEqual(radio.state("b"), "SLEEP")
        self.assertEqual([o for n, o, _ in observed if n == "b"], [])
        engine, radio, observed, _ = self.network({"nodes": nodes("ab"),
                                                  "default_link": {"power_dbm": -80}},
                                                 {"irq_delay_ns": 100_000_000})
        engine.schedule(0, DECIDE, radio.transmit, "a", b"1" * 25)
        engine.schedule(50_000_000, DECIDE, radio.transmit, "a", b"2" * 25)
        engine.run(200_000_000)
        self.assertEqual([o.payload for n, o, _ in observed if n == "b" and o.kind == "RX_DONE"],
                         [b"1" * 25, b"2" * 25])
        # Synthetic 4 dB threshold: either -85 dBm interferer alone is tolerable for -80 dBm,
        # while their summed power is not. These numbers are test inputs, not SX1262 calibration.
        for count in (1, 2):
            with self.subTest(interferers=count):
                sources = "ac" if count == 1 else "acd"
                engine, radio, observed, _ = self.network({"nodes": nodes("abcd"),
                    "noise": {"background_dbm": -200},
                    "links": [{"from": source, "to": "b", "power_dbm": -80 if source == "a" else -85}
                              for source in sources]},
                    {"reception_model": "capture", "capture_threshold_db": 4})
                for source in sources:
                    engine.schedule(0, DECIDE, radio.transmit, source, source.encode() * 25)
                engine.run(100_000_000)
                self.assertEqual([o.payload for n, o, _ in observed if n == "b" and o.kind == "RX_DONE"],
                                 [b"a" * 25] if count == 1 else [])
        engine, radio, _, _ = self.network({"nodes": nodes("ab"), "default_link": {"decode": True}},
                                           {"reception_model": "energy", "capture_threshold_db": 4})
        engine.schedule(0, DECIDE, radio.transmit, "a", b"x" * 25)
        with self.assertRaisesRegex(ValueError, "require every received power"):
            engine.run(100_000_000)
        for invalid in ({"noise_threshhold_dbm": -100}, {"implicit_payload_length": -1}):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                self.network({}, invalid)


if __name__ == "__main__":
    unittest.main()
