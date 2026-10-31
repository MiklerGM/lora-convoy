#!/usr/bin/env python3
"""Check the comparison contract against the actual firmware C simulator."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import compare


class ComparisonTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-C", str(compare.ROOT / "firmware"), "build/host/sim"],
                       check=True, capture_output=True, text=True)

    def run_scenario(self, directory, name, text, environment=None):
        scenario = directory / f"{name}.scenario"
        scenario.write_text(text)
        command = [str(compare.SIM), "--compare", str(scenario), "17"]
        if environment is not None:
            settings = directory / "conditions.environment"
            settings.write_text(environment)
            command.append(str(settings))
        result = subprocess.run(command,
                                check=True, capture_output=True, text=True)
        log = directory / f"{name}.jsonl"
        log.write_text(result.stdout)
        run, summary = compare.read_run(log, name, 17)
        events = [json.loads(line) for line in result.stdout.splitlines()]
        return log, run, summary, events

    def test_runner_keeps_environments_separate_and_preserves_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "results"
            command = [sys.executable, str(compare.ROOT / "tools/compare.py"),
                       "--scenario", "dense-10", "--environment", "ideal",
                       "--environment", "bursty-channel", "--seeds", "17,29", "--output", str(output)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            results = output / "results.json"
            original = results.read_bytes()
            data = json.loads(original)
            self.assertEqual(len(data["runs"]), 4)
            rows = {row["environment"]: row for row in data["scenarios"]}
            self.assertEqual(set(rows), {"ideal", "bursty-channel"})
            self.assertEqual([rows[name]["runs"] for name in sorted(rows)], [2, 2])
            self.assertEqual(rows["ideal"]["rx_external_interference"], 0)
            self.assertGreater(rows["bursty-channel"]["rx_external_interference"], 0)
            for run in data["runs"]:
                metadata, summary = compare.read_run(output / run["events"], "dense-10", run["seed"], run["environment"])
                self.assertEqual(metadata["environment"], run["environment"])
                self.assertEqual(summary, run["summary"])
            for environment in rows:
                name = f"{environment}.environment"
                self.assertEqual((output / "environments" / name).read_bytes(),
                                 (compare.ENVIRONMENTS / name).read_bytes())
            self.assertIn("dense-10 / bursty-channel", (output / "report.md").read_text())
            rerun = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(rerun.returncode, 2)
            self.assertIn("previous evidence is not overwritten", rerun.stderr)
            self.assertEqual(results.read_bytes(), original)

    def test_channel_loss_is_reproducible_and_distinct_from_missing_links(self):
        text = """name channel
nodes 2
interval_ms 4000
duration_s 120
warmup_s 20
clique 0 1
"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for directive, reason in (("loss_permille 1000", "rx_loss"),
                                      ("interference 1000 1000", "rx_external_interference")):
                with self.subTest(directive=directive):
                    _, _, summary, _ = self.run_scenario(
                        directory, "channel", text, f"environment blocked\n{directive}\n")
                    self.assertGreater(summary["rx_attempts"], 0)
                    self.assertEqual(summary[reason], summary["rx_attempts"])
                    self.assertEqual(summary["rx_accepted"], 0)
                    self.assertEqual(summary["path_unavailable_fraction"], 0)
                    self.assertEqual(summary["unseen_fraction"], 1)
            environment = "environment intermittent\ninterference 100 20\n"
            log, run, summary, events = self.run_scenario(directory, "channel", text, environment)
            first = log.read_bytes()
            log, _, _, _ = self.run_scenario(directory, "channel", text, environment)
            self.assertEqual(log.read_bytes(), first)
        self.assertGreater(summary["rx_external_interference"], 0)
        self.assertGreater(summary["rx_accepted"], 0)
        phase = run["interference_phase_us"]
        transmissions = {e["t_us"] + e["air_us"]: e for e in events if e["event"] == "tx"}
        straddles = 0
        for event in (e for e in events if e["event"] == "rx"):
            packet = transmissions[event["t_us"]]
            # Independent interval oracle, including a busy window starting inside a packet.
            start = packet["t_us"]
            stop = event["t_us"]
            busy = any(start < phase + k * 100000 + 20000 and
                       stop > phase + k * 100000 for k in range(-1, 1202))
            straddles += (start - phase) % 100000 >= 20000 and (start - phase) % 100000 + packet["air_us"] > 100000
            self.assertEqual(event["reason"], "external_interference" if busy else "accepted")
        self.assertGreater(straddles, 0)

    def test_position_loss_keeps_time_but_gnss_loss_eventually_holds_transmission(self):
        text = """name holdover
nodes 2
interval_ms 200
duration_s 600
warmup_s 20
clique 0 1
"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            _, fix_run, fix_summary, fix_events = self.run_scenario(
                directory, "holdover", text, "environment fix-only\nfix_outage 30 500 0\n")
            _, _, dark_summary, dark_events = self.run_scenario(
                directory, "holdover", text, "environment no-gnss\ngnss_outage 30 500 0\n")
            clock_text = text.replace("interval_ms 200\n", "interval_ms 4000\n").replace(
                "duration_s 600\n", "duration_s 120\n")
            _, _, wrong_clock, _ = self.run_scenario(
                directory, "holdover", clock_text, "environment underestimated\nnmea_only 1\nnmea_error_us 0\n")
            _, _, calibrated_clock, _ = self.run_scenario(
                directory, "holdover", clock_text,
                "environment calibrated\nnmea_only 1\nnmea_told_lag_us 100000\nnmea_error_us 1000\n")
            _, _, drifting_clock, drifting_events = self.run_scenario(
                directory, "drifting", "name drifting\nnodes 2\ninterval_ms 320\n"
                "duration_s 3600\nwarmup_s 3000\nlink 0 1\n",
                "environment drifting\ndrift_ppm 1000\ngnss_outage 30 3600 1\n")
        def sends(events, after, before):
            return [e for e in events if e["event"] == "tx" and e["sender"] == 0 and
                    after * 1000000 <= e["t_us"] < before * 1000000]
        self.assertGreater(len(sends(fix_events, 400, 500)), 400)
        self.assertEqual(fix_summary["tx_unsure"], 0)
        self.assertGreater(len(sends(dark_events, 40, 100)), 200)
        self.assertEqual(sends(dark_events, 400, 500), [])
        self.assertGreater(len(sends(dark_events, 505, 600)), 400)
        self.assertGreater(dark_summary["tx_unsure"], 0)
        expected_fix_us = fix_run["utc0_ms"] * 1000 + 29000000
        held = [e["fix_time_us"] for e in fix_events if e["event"] == "applied" and
                e["source"] == 0 and 100000000 < e["t_us"] < 499000000]
        self.assertGreater(len(held), 1000)
        self.assertEqual(set(held), {expected_fix_us})
        # Passing the core's declared-error gate is not proof that its real clock meets the bound.
        self.assertEqual(wrong_clock["clock_error_max_us"], 100000)
        self.assertGreater(wrong_clock["clock_bound_exceeded"], 0)
        self.assertEqual(wrong_clock["contract_failures"], 0)
        self.assertEqual(calibrated_clock["clock_error_max_us"], 0)
        self.assertEqual(calibrated_clock["clock_bound_exceeded"], 0)
        self.assertEqual(drifting_clock["contract_failures"], 0)
        self.assertLess(drifting_clock["age_p50_ms"], 0)
        self.assertLess(drifting_clock["age_p95_ms"], 0)
        self.assertTrue(any(e["event"] == "sample" and e["receiver"] == 1 and
                            e["age_ms"] is not None and e["age_ms"] < 0 for e in drifting_events))

    def test_directed_delivery_and_missing_states_are_not_hidden(self):
        text = """name directed
nodes 2
interval_ms 4000
duration_s 120
warmup_s 20
link 1 0
help 30 0 1
help 50 0 0
"""
        with tempfile.TemporaryDirectory() as temporary:
            _, run, summary, events = self.run_scenario(Path(temporary), "directed", text)
        applied = {(e["receiver"], e["source"]) for e in events if e["event"] == "applied"}
        self.assertEqual(applied, {(0, 1)})
        self.assertAlmostEqual(summary["unseen_fraction"], .5)
        self.assertEqual(summary["state_expected"], 2)
        self.assertEqual(summary["state_delivered"], 0)
        self.assertEqual(summary["state_superseded"], 1)
        self.assertEqual(summary["state_pending"], 1)
        self.assertIsNone(summary["state_delay_p95_ms"])
        rows = compare.aggregate([{"candidate": run["candidate"], "scenario": "directed", "summary": summary}])
        self.assertIsNone(rows[0]["state_delay_p95_ms"])
        self.assertEqual(rows[0]["state_delay_p95_ms_runs_without_samples"], 1)
        with tempfile.TemporaryDirectory() as temporary:
            _, _, summary, _ = self.run_scenario(Path(temporary), "directed", text.replace("link 1 0\n", ""))
        self.assertEqual(summary["unseen_fraction"], 1)
        self.assertEqual(summary["age_known_samples"], 0)
        self.assertIsNone(summary["age_p95_ms"])

    def test_partition_accounts_for_updates_and_waits_for_new_packets(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            fixture = (compare.SCENARIOS / "partition-5.scenario").read_text()
            _, _, summary, _ = self.run_scenario(directory, "partition-5", fixture)
            self.assertEqual(summary["state_expected"], 16)
            self.assertEqual(summary["state_delivered"], 4)
            self.assertEqual(summary["state_superseded"], 9)
            self.assertEqual(summary["state_pending"], 3)
            self.assertEqual(summary["contract_failures"], 0)
            text = """name reunion
nodes 2
interval_ms 4000
duration_s 120
warmup_s 20
clique 0 1
cut 40 80 0 1
help 50 0 1
help 60 0 0
"""
            _, _, summary, events = self.run_scenario(directory, "reunion", text)
        # The receiver's old normal state is not evidence that the later cancellation arrived.
        self.assertEqual(summary["state_expected"], 2)
        self.assertEqual(summary["state_delivered"], 1)
        self.assertEqual(summary["state_superseded"], 1)
        self.assertEqual(summary["state_pending"], 0)
        self.assertGreaterEqual(summary["state_delay_p95_ms"], 20000)
        cancellation = [e for e in events if e["event"] == "applied" and
                        e["receiver"] == 1 and e["source"] == 0 and e.get("state_event") == 1]
        self.assertTrue(cancellation)
        self.assertGreaterEqual(min(e["t_us"] for e in cancellation), 80000000)

    def test_bad_inputs_and_incomplete_results_are_rejected(self):
        good = """name invalid
nodes 2
interval_ms 4000
duration_s 120
warmup_s 20
clique 0 1
"""
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for directive in ("link 0 2\n", "unknown 1\n", "help 200 0 1\n"):
                with self.subTest(directive=directive):
                    scenario = directory / "invalid.scenario"
                    scenario.write_text(good + directive)
                    result = subprocess.run([str(compare.SIM), "--compare", str(scenario), "17"],
                                            capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertNotIn('"event":"summary"', result.stdout.replace(" ", ""))
            scenario.write_text(good)
            for directive in ("", "environment x\nloss_permille 10\nloss_permille 20\n",
                              "environment x\nnmea_lag_us 100\nnmea_jitter_us 101\n",
                              "environment x\ngnss_outage 20 121 all\n",
                              "environment x\nfix_outage 20 40 2\n",
                              "environment x\ninterference 1000 1001\n",
                              "environment x\nunknown 1\n"):
                with self.subTest(environment=directive):
                    environment = directory / "bad.environment"
                    environment.write_text(directive)
                    result = subprocess.run([str(compare.SIM), "--compare", str(scenario), "17", str(environment)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 2)
                    self.assertNotIn('"event":"summary"', result.stdout.replace(" ", ""))
            log, _, _, _ = self.run_scenario(directory, "invalid", good)
            lines = log.read_text().splitlines()
            log.write_text("\n".join(lines[:-1]) + "\n")
            with self.assertRaisesRegex(ValueError, "missing run or summary"):
                compare.read_run(log, "invalid", 17)


if __name__ == "__main__":
    unittest.main()
