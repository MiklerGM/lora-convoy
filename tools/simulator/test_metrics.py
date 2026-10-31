"""Independent analytic examples for observed ages, provenance and causal replay."""
from html.parser import HTMLParser
import json
import math
from pathlib import Path
import tempfile
import unittest

from .metrics import Evaluator, contact_reference
from .replay import write_replay
from .types import NS, Record


class MetricsTest(unittest.TestCase):
    def test_position_error_integrates_turns_and_actual_noisy_view_without_inventing_fix_age(self):
        def truth(author, at_ns):
            t = at_ns / NS
            if t <= 2:
                return (t, 0, 100)
            if t <= 4:
                return (2, t - 2, 100)
            if t <= 8:
                return (2, 2, 100)
            return (10 - t, 2, 100)

        def knots(author, begin, end):
            return [at for at in (2 * NS, 4 * NS, 8 * NS) if begin < at < end]

        e = Evaluator({"A": "g", "B": "g"}, 10 * NS,
                      truth_position=truth, truth_breakpoints=knots)
        e.measurement("fix", "A", 0, (0, 0, 0))
        e.source_record(Record("r", "A", 0, 0, "fix", None, (0, 0, 0), "normal"))
        e.applied("B", "r", 0, position=(1, 1, 0), state="normal")
        e.forget("B", 6 * NS)
        e.applied("B", "r", 8 * NS, position=(0, 2, 0), state="normal")
        result = e.finish()
        pair = next(p for p in result["pairs"] if p["receiver"] == "B")
        error = pair["position_error"]
        # XY integral: (8/3 + 8/3 + 4) over 0..6 s, then 8/3 over 8..10 s = 12 m² s.
        # Truth altitude 100 m is deliberately absent from the reported coordinates.
        self.assertEqual(error["available_ns"], 8 * NS)
        self.assertEqual(error["unavailable_ns"], 2 * NS)
        self.assertAlmostEqual(error["mean_squared_m2"], 1.5)
        self.assertAlmostEqual(error["rms_m"], math.sqrt(1.5))
        self.assertEqual(error["max_m"], 2)
        self.assertEqual(pair["true_age"]["known_ns"], 0)
        self.assertEqual(pair["unknown_provenance_ns"], 8 * NS)
        self.assertEqual(result["overall"]["position_error"]["coverage_fraction"], .4)
        self.assertEqual(result["overall"]["position_error"]["unavailable_ns"], 12 * NS)
        self.assertAlmostEqual(result["groups"][0]["position_error"]["mean_squared_m2"], 1.5)
        unsupported = Evaluator({"A": "g", "B": "g"}, NS, truth_position=truth)
        unsupported.applied("B", "unknown", 0, author="A", position=(0, 0, 0))
        missing = unsupported.finish()["overall"]["position_error"]
        self.assertEqual(missing["available_ns"], 0)
        self.assertIsNone(missing["rms_m"])

    def test_exact_duration_weighted_age_coverage_and_three_different_gaps(self):
        epoch = 1_000_000 * NS
        e = Evaluator({"A": "ride", "B": "ride", "X": "other"}, 10 * NS, utc0_ns=epoch)
        e.measurement("fix", "A", 0, (0, 0, 0))
        e.source_record(Record("r0", "A", 0, 0, "fix", None, (0, 0, 0), "normal"))
        e.calendar("B", 0, epoch)
        e.radio_received("B", 2 * NS, "A")
        e.applied("B", "r0", 2 * NS, display_fix_ns=epoch)
        e.radio_received("B", 4 * NS, "A")
        e.applied("B", "r0", 4 * NS, display_fix_ns=epoch)
        e.source_record(Record("r1", "A", 0, 4 * NS, "fix", None, (0, 0, 0), "normal"))
        e.calendar("B", 5 * NS, epoch + 5 * NS)
        e.applied("B", "r1", 5 * NS, display_fix_ns=epoch)
        summary = e.finish()
        self.assertEqual(summary["overall"]["pair_count"], 2)
        self.assertEqual(summary["overall"]["observation_ns"], 20 * NS)
        self.assertEqual(summary["overall"]["coverage_fraction"], .4)
        pair = next(p for p in summary["pairs"] if p["receiver"] == "B")
        age = pair["true_age"]
        self.assertEqual(age["known_ns"], 8 * NS)
        self.assertEqual(age["mean_ns"], 6 * NS)
        self.assertEqual(age["p50_ns"], 6 * NS)
        self.assertAlmostEqual(age["p95_ns"], 9.6 * NS)
        self.assertEqual(age["cdf"]["knots"], [
            {"value_ns": 2 * NS, "before_ns": 0, "at_ns": 0},
            {"value_ns": 10 * NS, "before_ns": 8 * NS, "at_ns": 8 * NS}])
        self.assertEqual(pair["displayed_age"]["mean_ns"], 6 * NS)
        self.assertEqual(pair["age_error"]["mean_ns"], 0)
        gaps = pair["gaps"]
        self.assertEqual(gaps["radio"]["unfinished_ns"], 6 * NS)
        self.assertEqual(gaps["new_record"]["unfinished_ns"], 5 * NS)
        self.assertEqual(gaps["new_fix"]["unfinished_ns"], 8 * NS)
        self.assertTrue(gaps["new_fix"]["longest_observed"]["right_censored"])
        unknown = next(p for p in summary["pairs"] if p["receiver"] == "A")
        self.assertEqual(unknown["true_age"]["known_ns"], 0)
        self.assertIsNone(unknown["true_age"]["p95_ns"])
        self.assertEqual(unknown["gaps"]["new_fix"]["longest_observed"]["duration_ns"], 10 * NS)

    def test_same_valued_states_do_not_prove_delivery_and_bad_clocks_do_not_improve_true_age(self):
        e = Evaluator({"A": "g", "B": "g"}, 12 * NS)
        e.measurement("fix", "A", 0, (0, 0, 0))
        e.state("normal0", "A", "normal", 0)
        e.source_record(Record("old", "A", 0, 0, "fix", "normal0", (0, 0, 0), "normal"))
        e.calendar("B", 0, -2 * NS)
        e.applied("B", "old", NS, display_fix_ns=0)
        e.state("help", "A", "help", 3 * NS)
        e.source_record(Record("help", "A", 0, 3 * NS, "fix", "help", (0, 0, 0), "help"))
        e.state("normal1", "A", "normal", 5 * NS)
        e.source_record(Record("new", "A", 0, 5 * NS, "fix", "normal1", (0, 0, 0), "normal"))
        e.applied("B", "unresolvable", 6 * NS, author="A", position=(0, 0, 0),
                  state="normal", display_fix_ns=0)
        e.applied("B", "unattributed", 7 * NS)
        e.applied("B", "new", 8 * NS, display_fix_ns=0)
        e.applied("B", "help", 9 * NS, display_fix_ns=0)
        e.calendar("B", 10 * NS, None)
        e.state("fuel", "B", "fuel", 10 * NS, expires_ns=11 * NS)
        e.calendar("B", 11 * NS, 11 * NS)
        e.applied("B", "new", 11 * NS, display_fix_ns=0)
        e.state("pending", "B", "help", 11 * NS + NS // 2)
        summary = e.finish()
        pair = next(p for p in summary["pairs"] if p["receiver"] == "B")
        self.assertEqual(pair["true_age"]["known_ns"], 9 * NS)
        self.assertEqual(pair["unknown_provenance_ns"], 2 * NS)
        self.assertEqual(pair["unknown_calendar_ns"], NS)
        self.assertEqual(pair["negative_display_age_ns"], NS)
        self.assertEqual(pair["state_correct_ns"], 7 * NS)
        self.assertEqual(pair["state_correct_unproven_ns"], 3 * NS)
        self.assertEqual(pair["record_rollbacks"], 1)
        self.assertEqual(summary["unattributed_applications"], 1)
        states = summary["states"]
        self.assertEqual([states[k] for k in ("expected", "delivered", "superseded", "expired", "pending", "late")],
                         [5, 2, 1, 1, 1, 1])
        cancellation = next(o for o in states["outcomes"] if o["event_id"] == "normal1")
        self.assertEqual(cancellation["first_proven_ns"], 8 * NS)
        self.assertEqual(cancellation["delay_ns"], 3 * NS)
        observed = Evaluator({"A": "g", "B": "g"}, 2 * NS)
        observed.measurement("f", "A", 0, (0, 0, 0))
        observed.state("h", "A", "help", 0)
        observed.source_record(Record("r", "A", 0, 0, "f", "h", (0, 0, 0), "help"))
        observed.applied("B", "r", 0, position=(99, 0, 0), state="normal")
        actual = observed.finish()
        view = next(p for p in actual["pairs"] if p["receiver"] == "B")
        self.assertEqual(view["true_age"]["known_ns"], 0)
        self.assertEqual(view["unknown_provenance_ns"], 2 * NS)
        self.assertEqual(view["state_correct_ns"], 0)
        self.assertEqual(view["provenance_mismatches"], 1)
        self.assertEqual(view["final_view"]["position"], (99, 0, 0))
        self.assertEqual(view["final_view"]["state"], "normal")
        self.assertEqual(actual["states"]["delivered"], 0)
        epoch = 1_792_670_400_000_000_000
        precise = Evaluator({"A": "g", "B": "g"}, 4 * NS, utc0_ns=epoch)
        precise.measurement("f", "A", 0, (0, 0, 0))
        precise.source_record(Record("r", "A", 0, 0, "f", None, (0, 0, 0), "normal"))
        precise.calendar("B", 0, epoch + 1, rate=1.0)
        precise.applied("B", "r", 0, display_fix_ns=epoch)
        precise.calendar("B", 2 * NS, epoch + 2 * NS + 1, rate=1.0)
        result = precise.finish()
        self.assertEqual(next(r for r in result["receivers"] if r["receiver"] == "B")["error"]["mean_ns"], 1)
        self.assertEqual(next(p for p in result["pairs"] if p["receiver"] == "B")["age_error"]["mean_ns"], 1)
        reboot = Evaluator({"A": "g", "B": "g"}, 8 * NS)
        reboot.measurement("f", "A", 0, (0, 0, 0))
        reboot.source_record(Record("r", "A", 0, 0, "f", None, (0, 0, 0), "normal"))
        reboot.applied("B", "r", NS)
        reboot.radio_received("B", NS, "A")
        reboot.forget("B", 3 * NS)
        reboot.radio_received("B", 5 * NS, "A")
        reboot.applied("B", "r", 5 * NS)
        after_reboot = next(p for p in reboot.finish()["pairs"] if p["receiver"] == "B")
        self.assertEqual(after_reboot["true_age"]["known_ns"], 5 * NS)
        self.assertEqual(after_reboot["view_ns"], 5 * NS)
        self.assertEqual(after_reboot["gaps"]["new_record"]["unfinished_ns"], 7 * NS)
        self.assertEqual(after_reboot["gaps"]["radio"]["unfinished_ns"], 3 * NS)

    def test_contact_reference_cannot_carry_a_record_created_after_rf_start_and_replay_is_inert(self):
        records = [Record("r0", "A", 0, 0, "f0", None, (0, 0, 0), "normal"),
                   Record("r1", "A", 0, 2 * NS, "f1", "help", (1, 0, 0), "help")]
        events = [
            {"event": "tx", "t_ns": NS, "end_ns": 4 * NS, "tx_id": "t0", "sender": "A"},
            {"event": "rx", "t_ns": 4 * NS, "tx_id": "t0", "sender": "A", "receiver": "B", "accepted": True},
            {"event": "tx", "t_ns": 4 * NS, "end_ns": 6 * NS, "tx_id": "t1", "sender": "B"},
            {"event": "rx", "t_ns": 6 * NS, "tx_id": "t1", "sender": "B", "receiver": "C", "accepted": True},
            {"event": "tx", "t_ns": 6 * NS, "end_ns": 7 * NS, "tx_id": "t2", "sender": "A"},
            {"event": "rx", "t_ns": 7 * NS, "tx_id": "t2", "sender": "A", "receiver": "B", "accepted": True},
            {"event": "tx", "t_ns": 7 * NS, "end_ns": 9 * NS, "tx_id": "t3", "sender": "B"},
            {"event": "rx", "t_ns": 9 * NS, "tx_id": "t3", "sender": "B", "receiver": "C", "accepted": True}]
        reference = contact_reference(events, records)
        self.assertFalse(reference["physically_feasible"])
        self.assertEqual(reference["arrivals"]["C"], {"r0": 6 * NS, "r1": 9 * NS})
        with self.assertRaisesRegex(ValueError, "precedes RF completion"):
            contact_reference([events[0], {**events[1], "t_ns": 2 * NS}], records)
        attack = '</script><script>globalThis.__injected=1</script><img src=x onerror=alert(1)>'
        trace = [{"event": "positions", "t_ns": 0, "positions": {attack: [0, 0, 0]}},
                 {"event": "applied", "t_ns": 0, "receiver": "B", "author": "A",
                  "record_key": "unknown", "position": None, "state": None},
                 {"event": "view_reset", "t_ns": 0, "receiver": "B"}, *events]

        class Document(HTMLParser):
            def __init__(self):
                super().__init__()
                self.in_script = False
                self.kind = None
                self.scripts = []
                self.images = []
            def handle_starttag(self, tag, attrs):
                if tag == "script":
                    self.in_script = True
                    self.kind = dict(attrs).get("type", "javascript")
                    self.scripts.append([self.kind, ""])
                if tag == "img":
                    self.images.append(dict(attrs))
            def handle_endtag(self, tag):
                if tag == "script":
                    self.in_script = False
            def handle_data(self, data):
                if self.in_script:
                    self.scripts[-1][1] += data

        with tempfile.TemporaryDirectory() as directory:
            path = write_replay(trace, Path(directory) / "replay.html")
            document = Document()
            document.feed(path.read_text())
        decoded = json.loads(next(text for kind, text in document.scripts if kind == "application/json"))
        self.assertEqual(decoded["events"], trace)
        self.assertEqual(document.images, [])
        self.assertFalse(any("globalThis.__injected" in text for kind, text in document.scripts
                             if kind != "application/json"))


if __name__ == "__main__":
    unittest.main()
