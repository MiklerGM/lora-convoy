"""Read-only, time-weighted observations; no evaluator identity is sent to a node."""
from collections import defaultdict
from dataclasses import asdict, dataclass, field
import math

from .types import Record

_UNOBSERVED = object()


class Distribution:
    """Duration spent at a linearly changing value, including point masses."""

    def __init__(self):
        self.segments = []
        self.total_ns = 0
        self.integral = 0.0

    def add(self, value, rate, duration_ns):
        if duration_ns <= 0:
            return
        if not math.isfinite(value) or not math.isfinite(rate):
            raise ValueError("a measured value and its rate must be finite")
        end = value + rate * duration_ns
        self.total_ns += duration_ns
        self.integral += (value + end) * .5 * duration_ns
        if self.segments:
            previous, slope, duration = self.segments[-1]
            if slope == rate and abs(previous + slope * duration - value) < 1e-6:
                self.segments[-1] = previous, slope, duration + duration_ns
                return
        self.segments.append((value, rate, duration_ns))

    def extend(self, other):
        self.segments.extend(other.segments)
        self.total_ns += other.total_ns
        self.integral += other.integral

    def duration_below(self, value):
        result = 0.0
        for start, rate, duration in self.segments:
            low, high = sorted((start, start + rate * duration))
            if low == high:
                result += duration if low <= value else 0
            else:
                result += duration * min(1.0, max(0.0, (value - low) / (high - low)))
        return result

    def summary(self, denominator_ns):
        changes, atoms = defaultdict(float), defaultdict(float)
        for value, rate, duration in self.segments:
            low, high = sorted((value, value + rate * duration))
            if low == high:
                atoms[low] += duration
            else:
                density = duration / (high - low)
                changes[low] += density
                changes[high] -= density
        points = sorted(changes.keys() | atoms.keys())
        mass = density = 0.0
        previous = points[0] if points else 0
        knots, quantiles = [], {}
        for point in points:
            before = mass + density * (point - previous)
            for percentile in (50, 95):
                target = self.total_ns * percentile / 100
                if percentile not in quantiles and target <= before and density > 0:
                    quantiles[percentile] = previous + (target - mass) / density
            mass = before + atoms[point]
            for percentile in (50, 95):
                if percentile not in quantiles and self.total_ns * percentile / 100 <= mass:
                    quantiles[percentile] = point
            knots.append({"value_ns": point, "before_ns": before, "at_ns": mass})
            density += changes[point]
            previous = point
        return {"known_ns": self.total_ns,
                "coverage_fraction": self.total_ns / denominator_ns if denominator_ns else None,
                "mean_ns": self.integral / self.total_ns if self.total_ns else None,
                "p50_ns": quantiles.get(50), "p95_ns": quantiles.get(95),
                "minimum_ns": points[0] if points else None,
                "maximum_ns": points[-1] if points else None,
                "absolute_maximum_ns": max(abs(points[0]), abs(points[-1])) if points else None,
                "cdf": {"representation": "linear-between-knots-with-atoms",
                        "conditional_denominator_ns": self.total_ns,
                        "all_pairs_denominator_ns": denominator_ns, "knots": knots}}


class Gap:
    def __init__(self, warmup_ns):
        self.warmup = warmup_ns
        self.last = None
        self.completed = 0
        self.longest = None

    def _consider(self, end, right_censored):
        begin = max(self.warmup, self.last if self.last is not None else self.warmup)
        if end <= begin:
            return
        value = {"from_ns": begin, "to_ns": end, "duration_ns": end - begin,
                 "left_censored": self.last is None or self.last < self.warmup,
                 "right_censored": right_censored}
        value["lower_bound"] = value["left_censored"] or right_censored
        if self.longest is None or value["duration_ns"] > self.longest["duration_ns"]:
            self.longest = value

    def observe(self, at_ns):
        if self.last is not None and at_ns < self.last:
            raise ValueError("gap events must be chronological")
        if at_ns >= self.warmup:
            self._consider(at_ns, False)
            self.completed += self.last is not None and self.last >= self.warmup and at_ns > self.last
        self.last = at_ns

    def summary(self, end_ns):
        self._consider(end_ns, True)
        return {"complete_intervals": self.completed, "longest_observed": self.longest,
                "unfinished_ns": max(0, end_ns - max(self.warmup, self.last or self.warmup))}


@dataclass
class View:
    key: str | None
    fix_id: str | None
    state_event: str | None
    position: tuple | None
    state: str | None
    display_fix_ns: int | None


@dataclass
class PositionError:
    available_ns: int = 0
    integral_m2_ns: float = 0
    max_squared_m2: float | None = None

    def add_segment(self, shown, start, end, duration_ns):
        # The current wire/map view has no measured altitude; compare planar XY only.
        if any(point is None or len(point) < 2 or not all(math.isfinite(v) for v in point[:2])
               for point in (shown, start, end)):
            return
        a = tuple(p - q for p, q in zip(shown[:2], start[:2]))
        b = tuple(p - q for p, q in zip(shown[:2], end[:2]))
        square_a = math.fsum(value * value for value in a)
        square_b = math.fsum(value * value for value in b)
        cross = math.fsum(x * y for x, y in zip(a, b))
        integral = (square_a / 3 + cross / 3 + square_b / 3) * duration_ns
        if not all(math.isfinite(value) for value in (square_a, square_b, integral)):
            raise ValueError("position-error integral exceeds finite numeric range")
        self.available_ns += duration_ns
        self.integral_m2_ns += integral
        maximum = max(square_a, square_b)
        self.max_squared_m2 = maximum if self.max_squared_m2 is None else max(self.max_squared_m2, maximum)

    def extend(self, other):
        self.available_ns += other.available_ns
        self.integral_m2_ns += other.integral_m2_ns
        if other.max_squared_m2 is not None:
            self.max_squared_m2 = (other.max_squared_m2 if self.max_squared_m2 is None else
                                   max(self.max_squared_m2, other.max_squared_m2))

    def summary(self, denominator_ns, configured):
        mean = self.integral_m2_ns / self.available_ns if self.available_ns else None
        return {"available_ns": self.available_ns, "unavailable_ns": denominator_ns - self.available_ns,
                "coverage_fraction": self.available_ns / denominator_ns if denominator_ns else None,
                "mean_squared_m2": mean, "rms_m": math.sqrt(mean) if mean is not None else None,
                "max_m": math.sqrt(self.max_squared_m2) if self.max_squared_m2 is not None else None,
                "model": ("exact-piecewise-linear-XY-truth-held-observed-point" if configured else
                          "unavailable-without-truth-position-and-trajectory-breakpoints"),
                "quantiles": "not computed"}


@dataclass
class Pair:
    at_ns: int
    view: View | None = None
    view_ns: int = 0
    unknown_provenance_ns: int = 0
    unknown_calendar_ns: int = 0
    state_correct_ns: int = 0
    state_correct_unproven_ns: int = 0
    true_age: Distribution = field(default_factory=Distribution)
    displayed_age: Distribution = field(default_factory=Distribution)
    age_error: Distribution = field(default_factory=Distribution)
    position_error: PositionError = field(default_factory=PositionError)
    seen_records: set = field(default_factory=set)
    seen_fixes: set = field(default_factory=set)
    newest_record_ns: int = -1
    newest_fix_ns: int = -1
    record_rollbacks: int = 0
    fix_rollbacks: int = 0
    provenance_mismatches: int = 0


class Evaluator:
    def __init__(self, groups, duration_ns, warmup_ns=0, truth_position=None, emit=None, utc0_ns=0,
                 truth_breakpoints=None):
        if not 0 <= warmup_ns < duration_ns:
            raise ValueError("warmup must be shorter than the positive duration")
        self.groups = dict(groups)
        self.duration_ns, self.warmup_ns, self.utc0_ns = duration_ns, warmup_ns, utc0_ns
        self.truth_position, self.emit = truth_position, emit
        for callback in (truth_position, truth_breakpoints):
            if callback is not None and not callable(callback):
                raise ValueError("truth position and trajectory breakpoints must be callable")
        self.truth_breakpoints = truth_breakpoints
        self.position_error_configured = truth_position is not None and truth_breakpoints is not None
        self.now = 0
        self.fixes, self.records, self.states = {}, {}, {}
        self.current_states = {}
        self.calendars = {node: None for node in groups}
        self.clock_at = dict.fromkeys(groups, 0)
        self.clock_errors = {node: Distribution() for node in groups}
        self.pairs = {(receiver, author): Pair(0) for receiver in groups for author in groups
                      if receiver != author and groups[receiver] == groups[author]}
        self.gaps = {pair: {kind: Gap(warmup_ns) for kind in ("radio", "new_record", "new_fix")}
                     for pair in self.pairs}
        self.radio_any = {node: Gap(warmup_ns) for node in groups}
        self.unattributed_applications = self.outside_group_applications = 0
        self.result = None

    def advance(self, at_ns):
        if self.result is not None:
            raise ValueError("the observation has finished")
        if not self.now <= at_ns <= self.duration_ns:
            raise ValueError("observations must be chronological and within the run")
        self.now = at_ns

    def _flush_clock(self, receiver, until):
        begin = max(self.warmup_ns, self.clock_at[receiver])
        calendar = self.calendars[receiver]
        if until > begin and calendar is not None:
            at, utc, rate = calendar
            error = (utc - self.utc0_ns - at) + (begin - at) * (rate - 1)
            self.clock_errors[receiver].add(error, rate - 1, until - begin)
        self.clock_at[receiver] = until

    def _flush_pair(self, pair, until):
        receiver, author = pair
        p = self.pairs[pair]
        begin = max(self.warmup_ns, p.at_ns)
        p.at_ns = until
        if until <= begin or p.view is None:
            return
        duration = until - begin
        view = p.view
        p.view_ns += duration
        if self.position_error_configured and view.position is not None:
            knots = list(self.truth_breakpoints(author, begin, until))
            if any(type(at) is not int or not begin < at < until for at in knots):
                raise ValueError("trajectory breakpoints must be integer nanoseconds strictly inside the interval")
            boundaries = [begin, *sorted(set(knots)), until]
            for start, end in zip(boundaries, boundaries[1:]):
                p.position_error.add_segment(view.position, self.truth_position(author, start),
                                             self.truth_position(author, end), end - start)
        measurement = self.fixes.get(view.fix_id)
        if measurement is not None and measurement["author"] == author:
            p.true_age.add(begin - measurement["measured_ns"], 1, duration)
        else:
            p.unknown_provenance_ns += duration
        calendar = self.calendars[receiver]
        if calendar is not None and view.display_fix_ns is not None:
            at, utc, rate = calendar
            age = (utc - view.display_fix_ns) + (begin - at) * rate
            p.displayed_age.add(age, rate, duration)
            if measurement is not None:
                error = (utc - view.display_fix_ns - at + measurement["measured_ns"]) + (begin - at) * (rate - 1)
                p.age_error.add(error, rate - 1, duration)
        else:
            p.unknown_calendar_ns += duration
        current = self.current_states.get(author)
        state = self.states[current]["state"] if current is not None else "normal"
        if view.state == state:
            p.state_correct_ns += duration
            if current is not None and view.state_event != current:
                p.state_correct_unproven_ns += duration

    def measurement(self, fix_id, author, measured_ns, position):
        if author not in self.groups or not 0 <= measured_ns <= self.duration_ns:
            raise ValueError("measurement author or time is outside the run")
        value = {"author": author, "measured_ns": measured_ns, "position": tuple(position)}
        if fix_id in self.fixes and self.fixes[fix_id] != value:
            raise ValueError("measurement identity was reused")
        for pair, p in self.pairs.items():
            if p.view is not None and p.view.fix_id == fix_id:
                self._flush_pair(pair, self.now)
        self.fixes[fix_id] = value

    def state(self, event_id, author, state, at_ns, expires_ns=None):
        self.advance(at_ns)
        if author not in self.groups or event_id in self.states:
            raise ValueError("state author is unknown or event identity was reused")
        if expires_ns is not None and expires_ns <= at_ns:
            raise ValueError("state expiration must follow its creation")
        for pair in self.pairs:
            if pair[1] == author:
                self._flush_pair(pair, at_ns)
        if author in self.current_states:
            self.states[self.current_states[author]]["superseded_ns"] = at_ns
        self.states[event_id] = {"author": author, "state": state, "at_ns": at_ns,
                                 "expires_ns": expires_ns, "superseded_ns": None, "delivered": {}}
        self.current_states[author] = event_id

    def source_record(self, record: Record):
        if record.author not in self.groups or not 0 <= record.created_ns <= self.now:
            raise ValueError("record author or creation time is invalid")
        if record.key in self.records and self.records[record.key] != record:
            raise ValueError("record identity was reused")
        fix = self.fixes.get(record.fix_id)
        state = self.states.get(record.state_event)
        if fix is not None and (fix["author"] != record.author or fix["measured_ns"] > record.created_ns):
            raise ValueError("record refers to a foreign or future measurement")
        if state is not None and (state["author"] != record.author or state["at_ns"] > record.created_ns or
                                  state["state"] != record.state):
            raise ValueError("record refers to a foreign, future or different state")
        self.records[record.key] = record

    def applied(self, receiver, record_key, at_ns, display_fix_ns=None, via=None,
                author=None, position=_UNOBSERVED, state=_UNOBSERVED):
        self.advance(at_ns)
        record = self.records.get(record_key)
        if record is not None:
            if author is not None and author != record.author:
                raise ValueError("view author differs from the record's author")
            if record.created_ns > at_ns:
                raise ValueError("a record was applied before it existed")
            author = record.author
        if author is None:
            self.unattributed_applications += 1
            return
        pair = receiver, author
        if pair not in self.pairs:
            self.outside_group_applications += 1
            return
        self._flush_pair(pair, at_ns)
        p = self.pairs[pair]
        if record is None:
            p.view = View(record_key, None, None,
                          tuple(position) if position is not _UNOBSERVED and position is not None else None,
                          state if state is not _UNOBSERVED else None, display_fix_ns)
            return
        observed_position = record.position if position is _UNOBSERVED else tuple(position) if position is not None else None
        observed_state = record.state if state is _UNOBSERVED else state
        coordinate_match = observed_position == tuple(record.position)
        state_match = observed_state == record.state
        p.provenance_mismatches += not coordinate_match or not state_match
        p.view = View(record.key, record.fix_id if coordinate_match else None,
                      record.state_event if state_match else None, observed_position,
                      observed_state, display_fix_ns)
        if record.key not in p.seen_records:
            p.seen_records.add(record.key)
            if record.created_ns >= p.newest_record_ns:
                self.gaps[pair]["new_record"].observe(at_ns)
                p.newest_record_ns = record.created_ns
            else:
                p.record_rollbacks += 1
        fix = self.fixes.get(record.fix_id) if coordinate_match else None
        if fix is not None and record.fix_id not in p.seen_fixes:
            p.seen_fixes.add(record.fix_id)
            if fix["measured_ns"] >= p.newest_fix_ns:
                self.gaps[pair]["new_fix"].observe(at_ns)
                p.newest_fix_ns = fix["measured_ns"]
            else:
                p.fix_rollbacks += 1
        event = self.states.get(record.state_event) if state_match else None
        if event is not None:
            event["delivered"].setdefault(receiver, at_ns)

    def radio_received(self, receiver, at_ns, source=None):
        self.advance(at_ns)
        self.radio_any[receiver].observe(at_ns)
        if (receiver, source) in self.gaps:
            self.gaps[receiver, source]["radio"].observe(at_ns)

    def forget(self, receiver, at_ns, author=None):
        self.advance(at_ns)
        if receiver not in self.groups:
            raise ValueError("unknown receiver")
        for pair, state in self.pairs.items():
            if pair[0] == receiver and (author is None or pair[1] == author):
                self._flush_pair(pair, at_ns)
                state.view = None

    def calendar(self, receiver, at_ns, utc_ns, rate=1):
        self.advance(at_ns)
        if not math.isfinite(rate) or rate < 0:
            raise ValueError("calendar rate must be finite and nonnegative")
        self._flush_clock(receiver, at_ns)
        for pair in self.pairs:
            if pair[0] == receiver:
                self._flush_pair(pair, at_ns)
        self.calendars[receiver] = (at_ns, utc_ns, rate) if utc_ns is not None else None

    def _state_summary(self):
        outcomes, delays = [], []
        counts = dict.fromkeys(("expected", "delivered", "superseded", "expired", "pending", "late"), 0)
        events = list(self.states.items())
        for event_id, event in events:
            if event["at_ns"] < self.warmup_ns:
                continue
            next_at = event["superseded_ns"]
            boundaries = [(next_at, "superseded"), (event["expires_ns"], "expired")]
            boundary, reason = min(((at, why) for at, why in boundaries if at is not None),
                                   default=(None, None))
            for receiver, author in self.pairs:
                if author != event["author"]:
                    continue
                arrived = event["delivered"].get(receiver)
                if arrived is not None and (boundary is None or arrived < boundary):
                    outcome = "delivered"
                    delays.append(arrived - event["at_ns"])
                else:
                    outcome = reason if boundary is not None and boundary <= self.duration_ns else "pending"
                late = arrived is not None and outcome != "delivered"
                counts["expected"] += 1
                counts[outcome] += 1
                counts["late"] += late
                outcomes.append({"event_id": event_id, "author": author, "receiver": receiver,
                                 "outcome": outcome, "first_proven_ns": arrived, "late": late,
                                 "delay_ns": arrived - event["at_ns"] if outcome == "delivered" else None})
        delays.sort()
        return {**counts, "outcomes": outcomes,
                "delivered_delay_p50_ns": delays[math.ceil(len(delays) * .5) - 1] if delays else None,
                "delivered_delay_p95_ns": delays[math.ceil(len(delays) * .95) - 1] if delays else None}

    def finish(self):
        if self.result is not None:
            return self.result
        self.advance(self.duration_ns)
        window = self.duration_ns - self.warmup_ns
        rows, grouped = [], defaultdict(list)
        for pair, p in self.pairs.items():
            self._flush_pair(pair, self.duration_ns)
            row = {"receiver": pair[0], "author": pair[1], "group": self.groups[pair[0]],
                   "observation_ns": window, "view_ns": p.view_ns,
                   "unknown_provenance_ns": p.unknown_provenance_ns,
                   "unknown_calendar_ns": p.unknown_calendar_ns,
                   "state_correct_ns": p.state_correct_ns,
                   "state_correct_unproven_ns": p.state_correct_unproven_ns,
                   "true_age": p.true_age.summary(window), "displayed_age": p.displayed_age.summary(window),
                   "age_error": p.age_error.summary(window),
                   "position_error": p.position_error.summary(window, self.position_error_configured),
                   "negative_display_age_ns": p.displayed_age.duration_below(-1e-9),
                   "record_rollbacks": p.record_rollbacks, "fix_rollbacks": p.fix_rollbacks,
                   "provenance_mismatches": p.provenance_mismatches,
                   "final_view": asdict(p.view) if p.view is not None else None,
                   "gaps": {kind: gap.summary(self.duration_ns) for kind, gap in self.gaps[pair].items()}}
            rows.append(row)
            grouped[row["group"]].append((p, row))

        def aggregate(items):
            denominator = len(items) * window
            result = {"pair_count": len(items), "observation_ns": denominator}
            for name in ("view_ns", "unknown_provenance_ns", "unknown_calendar_ns", "state_correct_ns",
                         "state_correct_unproven_ns", "record_rollbacks", "fix_rollbacks"):
                result[name] = sum(row[name] for _, row in items)
            result["provenance_mismatches"] = sum(row["provenance_mismatches"] for _, row in items)
            for name in ("true_age", "displayed_age", "age_error"):
                values = Distribution()
                for p, _ in items:
                    values.extend(getattr(p, name))
                result[name] = values.summary(denominator)
            result["coverage_fraction"] = result["true_age"]["coverage_fraction"]
            positions = PositionError()
            for p, _ in items:
                positions.extend(p.position_error)
            result["position_error"] = positions.summary(denominator, self.position_error_configured)
            return result

        clocks = []
        for receiver in self.groups:
            self._flush_clock(receiver, self.duration_ns)
            clocks.append({"receiver": receiver, "error": self.clock_errors[receiver].summary(window),
                           "radio_any_gap": self.radio_any[receiver].summary(self.duration_ns)})
        self.result = {"schema": 1, "unit": "ns", "duration_ns": self.duration_ns,
                       "warmup_ns": self.warmup_ns, "pairs": rows, "receivers": clocks,
                       "overall": aggregate([(self.pairs[row["receiver"], row["author"]], row) for row in rows]),
                       "groups": [{"group": group, **aggregate(items)} for group, items in grouped.items()],
                       "states": self._state_summary(),
                       "unattributed_applications": self.unattributed_applications,
                       "outside_group_applications": self.outside_group_applications,
                       "not_measured": (["position error integral (truth position and trajectory breakpoints required)"]
                                        if not self.position_error_configured else []) +
                                       ["position error quantiles", "vertical position error", "energy consumption", "phone/map delivery"],
                       "semantics": "CDF masses are durations; normalize by known_ns for conditional ages or observation_ns for coverage-and-age. Boundary gaps are censored. State correctness does not prove delivery of a same-valued event."}
        return self.result


def contact_reference(events, source_records=()):
    """Unlimited-content diagnostic on successful RXs, not a feasible protocol or optimum."""
    scheduled = []
    for record in source_records:
        scheduled.append((record.created_ns, 0, "source_record", {"record": asdict(record)}))
    for event in events:
        kind = event.get("event")
        if kind in ("source_record", "tx", "rx"):
            scheduled.append((event["t_ns"], {"source_record": 0, "rx": 1, "tx": 2}[kind], kind, event))
    known, snapshots, arrivals = defaultdict(set), {}, defaultdict(dict)
    for at, _, kind, event in sorted(scheduled, key=lambda item: (item[0], item[1])):
        if kind == "source_record":
            value = event.get("record", event)
            key = value.get("key", value.get("record_key"))
            author = value.get("author", value.get("node"))
            if key is None or author is None:
                raise ValueError("contact reference needs source record identity and author")
            known[author].add(key)
            arrivals[author].setdefault(key, at)
        elif kind == "tx":
            if "tx_id" not in event:
                raise ValueError("contact reference needs an RF transmission identity")
            if event["tx_id"] in snapshots:
                raise ValueError("RF transmission identity was reused")
            if event.get("end_ns", at) <= at:
                raise ValueError("RF transmission must have a positive recorded duration")
            snapshots[event["tx_id"]] = frozenset(known[event["sender"]]), event["end_ns"]
        elif event.get("accepted", False):
            if event.get("tx_id") not in snapshots:
                raise ValueError("accepted reception has no preceding RF start")
            records, ended = snapshots[event["tx_id"]]
            if at < ended:
                raise ValueError("reception precedes RF completion")
            receiver = event["receiver"]
            for key in records:
                known[receiver].add(key)
                arrivals[receiver].setdefault(key, at)
    return {"kind": "unlimited-content-on-observed-receptions", "physically_feasible": False,
            "causality": "snapshot at RF start, apply only at successful RX end",
            "arrivals": dict(arrivals),
            "warning": "Free unlimited payload is a diagnostic envelope for this fixed trace. It is not an access/selection loss decomposition."}
