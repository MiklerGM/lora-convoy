"""Public timing, physical-input and validation contracts of the DES world."""
import copy
import unittest

from .kernel import ACQUIRE, COMPLETE, DECIDE, OBSERVE, SENSOR, START, WORLD, Engine
from .types import NS
from .world import World
from .radio import RadioNetwork


def scenario():
    return {"schema": 1, "name": "world-test", "duration_s": 10, "nodes": [
        {"id": "a", "group": "g", "slot": 0, "trajectory": [
            {"t_s": 0, "x_m": 0, "y_m": 0}, {"t_s": 10, "x_m": 10, "y_m": 0}]},
        {"id": "b", "group": "g", "slot": 1, "trajectory": [{"t_s": 0, "x_m": 20, "y_m": 0}]},
        {"id": "c", "group": "g", "slot": 2, "trajectory": [{"t_s": 0, "x_m": 30, "y_m": 0}]}],
        "default_link": {"decode": True, "sense": True, "interfere": True, "power_dbm": -80}}


class WorldTest(unittest.TestCase):
    def test_kernel_phases_causal_delta_cancellation_and_zero_time_limit(self):
        for reverse in (False, True):
            engine, observed = Engine(reverse_ties=reverse), []
            for name,phase in (("acquire",ACQUIRE),("start",START),("decide",DECIDE),
                               ("observe",OBSERVE),("sensor",SENSOR),("world",WORLD),("complete",COMPLETE)):
                def callback(phase=phase, name=name):
                    observed.append((engine.now_ns, name))
                    if phase == START:
                        engine.schedule(engine.now_ns, OBSERVE, observed.append, (engine.now_ns, "reaction"))
                engine.schedule(100, phase, callback)
            cancelled = engine.schedule(100, WORLD, self.fail, "cancelled event executed")
            cancelled.cancel()
            engine.run(100)
            self.assertEqual(observed, [(100, name) for name in
                             ("complete","world","sensor","observe","decide","start","acquire","reaction")])
            with self.assertRaises(ValueError):
                engine.schedule(99, WORLD, lambda: None)
        engine = Engine(same_time_limit=8)
        def loop():
            engine.schedule(engine.now_ns, COMPLETE, loop)
        engine.schedule(0, COMPLETE, loop)
        with self.assertRaisesRegex(RuntimeError, "without advancing time"):
            engine.run(1)

    def test_world_boundaries_sensors_clocks_and_seeded_channel_are_independent(self):
        config = scenario()
        config["nodes"][0]["clock"] = {"offset_us": 1024, "drift_ppm": -400, "resolution_us": 31}
        config["nodes"][0]["gnss"] = {"position_error_m": 2, "lag_ms": 100, "jitter_ms": 10}
        config["outages"] = [{"node": "a", "kind": "fix", "from_s": 1, "until_s": 2},
                              {"node": "a", "kind": "gnss", "from_s": 2, "until_s": 3}]
        config["zones"] = [{"id": "narrow-shadow", "lower_m": [4, -1], "upper_m": [6, 1],
                            "endpoint": "sender", "link": {"decode": False}}]
        config["noise"] = {"background_dbm": -120, "bursts": [
            {"from_s": .5, "until_s": 1.5, "receivers": ["b"], "power_dbm": -80}]}
        world = World(config, env_seed=17)
        self.assertEqual(world.position("a", 5 * NS), (5, 0, 0))
        self.assertEqual(world.velocity("a", 5 * NS), (1, 0, 0))
        self.assertTrue(world.link("a", "b", 3 * NS).decode)
        self.assertFalse(world.link("a", "b", 5 * NS).decode)
        self.assertTrue(world.link("a", "b", 7 * NS).decode)
        self.assertIn(4 * NS, world.boundaries())
        self.assertIn(6 * NS, world.boundaries())
        self.assertEqual(world.local_us("a", 2 * NS), 2000213)
        self.assertEqual(world.world_deadline("a", 2000214), 2000020009)
        world.reboot("a", 2 * NS)
        self.assertEqual(world.local_us("a", 2 * NS), 1023)
        self.assertEqual(world.world_deadline("a", 2000214), 4000020009)
        variable = scenario()
        variable["nodes"][0]["clock"] = {"offset_us": 1000, "profile": [
            {"from_s": 0, "drift_ppm": 1000}, {"from_s": 2, "drift_ppm": -1000},
            {"from_s": 5, "drift_ppm": 2000}]}
        temperature = World(variable)
        self.assertEqual([temperature.local_us("a", second * NS) for second in (1, 2, 3, 5, 6)],
                         [1002000, 2003000, 3002000, 5000000, 6002000])
        self.assertEqual(temperature.world_deadline("a", 3002001), 3000001002)
        self.assertEqual(temperature.world_deadline("a", 5000001), 5000000999)
        self.assertEqual(temperature.clock_rate("a", 2 * NS), .999)
        self.assertIn(2 * NS, temperature.boundaries())
        self.assertIn(5 * NS, temperature.boundaries())
        temperature.reboot("a", 4 * NS)
        self.assertEqual(temperature.local_us("a", 5 * NS), 1000000)
        self.assertEqual(temperature.local_us("a", 6 * NS), 2002000)
        self.assertEqual(temperature.world_deadline("a", 1000001), 5000000999)
        fix_only = world.gnss("a", 1, NS + NS // 2)
        absent = world.gnss("a", 2, 2 * NS + NS // 2)
        self.assertEqual((fix_only["valid"], fix_only["time_valid"], fix_only["pps"]), (False, True, True))
        self.assertEqual((absent["present"], absent["valid"], absent["time_valid"], absent["pps"]), (False,) * 4)
        measured = world.gnss("a", 7, 5 * NS)
        world.gnss("b", 99, 8 * NS)
        self.assertEqual(world.gnss("a", 7, 5 * NS), measured)
        self.assertLessEqual(sum((a-b)**2 for a, b in zip(measured["position"], (5, 0, 0))), 4)
        self.assertGreaterEqual(measured["available_ns"], 5 * NS + 90000000)
        self.assertLessEqual(measured["available_ns"], 5 * NS + 110000000)
        self.assertAlmostEqual(world.noise_dbm("b", NS), -79.99956572723137)
        self.assertEqual(world.noise_dbm("a", NS), -120)
        self.assertEqual(world.noise_dbm("b", 2 * NS), -120)
        fading = scenario()
        fading["fading"] = {"grid_s": .25, "std_db": 3, "correlation_s": 2, "scope": "receiver"}
        first, second = World(fading, env_seed=17), World(fading, env_seed=17)
        timestamps = [3 * NS, NS, 0, 2 * NS]
        powers = {t: first.link("a", "b", t).power_dbm for t in timestamps}
        for t in reversed(timestamps):
            self.assertEqual(second.link("a", "b", t).power_dbm, powers[t])
            self.assertEqual(second.link("c", "b", t).power_dbm, powers[t])
        self.assertGreater(len(set(powers.values())), 1)
        self.assertNotEqual(World(fading, env_seed=29).link("a", "b", 3 * NS).power_dbm, powers[3 * NS])
        geometric = scenario()
        geometric.pop("default_link")
        geometric["propagation"] = {"reference_distance_m": 1, "reference_power_dbm": -40,
                                      "path_loss_exponent": 2, "min_distance_m": 1}
        geometric["max_channel_update_s"] = 1
        model = World(geometric)
        self.assertAlmostEqual(model.link("a", "b", 0).power_dbm, -66.02059991327963)
        self.assertAlmostEqual(model.link("a", "b", 5 * NS).power_dbm, -63.52182518111363)
        self.assertEqual(model.link("a", "b", 5100000000).power_dbm, model.link("a", "b", 5900000000).power_dbm)
        # Both endpoints of a movement segment can hide an internal contact change.
        for start_x, stop_x, transmit_ns, expected in ((0, 3, 330000000, "RX_DONE"),
                                                       (2, 0, 490000000, "RX_ERROR")):
            with self.subTest(shadow_direction=(start_x, stop_x)):
                shadow = scenario()
                shadow["duration_s"] = 1
                shadow["nodes"] = shadow["nodes"][:2]
                shadow["nodes"][0]["trajectory"] = [
                    {"t_s": 0, "x_m": start_x, "y_m": 0},
                    {"t_s": 1, "x_m": stop_x, "y_m": 0}]
                shadow["zones"] = [{"id": "edge", "lower_m": [0, -1], "upper_m": [1, 1],
                                    "endpoint": "sender", "link": {"decode": False}}]
                engine, channel, observed = Engine(), World(shadow), []
                radio = RadioNetwork(engine, channel, lambda node, event: observed.append((node, event)), lambda event: None)
                for node in channel.node_ids:
                    radio.add_node(node, channel.phy(node))
                for boundary in channel.boundaries():
                    engine.schedule(boundary, WORLD, radio.environment_changed)
                engine.schedule(transmit_ns, DECIDE, radio.transmit, "a", b"x" * 25)
                engine.run(NS)
                self.assertEqual([(event.kind, event.at_ns) for node, event in observed
                                  if node == "b" and event.kind in ("RX_DONE", "RX_ERROR")],
                                 [(expected, transmit_ns + 30848000)])

    def test_world_validates_group_local_slots_references_and_model_contracts(self):
        config = scenario()
        config["nodes"] = [{"id": f"n{i}", "group": f"g{i//32}", "slot": i % 32,
                            "trajectory": [{"t_s": 0, "x_m": i, "y_m": 0}]} for i in range(65)]
        self.assertEqual(len(World(config).nodes), 65)
        invalid = []
        value = scenario(); value["nodes"][1]["slot"] = 0; invalid.append(value)
        value = scenario(); value["links"] = [{"from": "a", "to": "missing"}]; invalid.append(value)
        value = scenario(); value["nodes"][0]["trajectory"][0]["x_m"] = float("nan"); invalid.append(value)
        value = scenario(); value["nodes"][0]["clock"] = {"offset_us": -1}; invalid.append(value)
        value = scenario(); value["actions"] = [{"t_s": 1, "node": "a", "kind": "state", "state": "fuel"}]; invalid.append(value)
        value = scenario(); value["schema"] = True; invalid.append(value)
        value = scenario(); value["name"] = "../../overwritten"; invalid.append(value)
        value = scenario(); value["nodes"][0]["id"] = "all"; invalid.append(value)
        value = scenario(); value["nodes"][0]["clock"] = {"profile": [{"from_s": 1, "drift_ppm": 0}]}; invalid.append(value)
        value = scenario(); value["nodes"][0]["clock"] = {"profile": [{"from_s": 0, "drift_ppm": 0}, {"from_s": 0, "drift_ppm": 1}]}; invalid.append(value)
        value = scenario(); value["fading"] = {"grid_s": 1, "std_db": 3, "correlation_s": 2, "scope": "global"}; value["default_link"].pop("power_dbm"); invalid.append(value)
        for case in invalid:
            with self.subTest(case=case):
                with self.assertRaises(ValueError):
                    World(copy.deepcopy(case))


if __name__ == "__main__":
    unittest.main()
