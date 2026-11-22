"""Validated scenario inputs and pure world/sensor queries, independent of protocol calls."""
import bisect
import copy
import hashlib
import json
import math
import re
from fractions import Fraction

from .types import Link, NS, Phy, US

NOISE_PROCESS_LIMIT = 256
NOISE_TRANSITION_LIMIT = 1_000_000


def _keys(value, allowed, where):
    if not isinstance(value, dict):
        raise ValueError(f"{where} must be an object")
    unknown = set(value) - set(allowed)
    if unknown:
        raise ValueError(f"{where}: unknown fields {sorted(unknown)}")


def _number(value, where, low=-math.inf, high=math.inf):
    if type(value) not in (float, int) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"{where} must be finite and between {low} and {high}")
    return value


def _integer(value, where, low=0, high=2**63 - 1):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{where} must be an integer between {low} and {high}")
    return value


def _name(value, where):
    if not isinstance(value, str) or len(value) > 128 or not re.fullmatch(r"[A-Za-z0-9_.-]+", value):
        raise ValueError(f"{where} must be an identifier of 1..128 ASCII letters, digits, '.', '_' or '-'")
    return value


def _boolean(value, where):
    if type(value) is not bool:
        raise ValueError(f"{where} must be boolean")
    return value


def _list(value, where):
    if not isinstance(value, list):
        raise ValueError(f"{where} must be a list")
    return value


def _ns(seconds):
    return round(seconds * NS)


def _phy(raw, base=None):
    _keys(raw, Phy.__dataclass_fields__, "phy")
    values = vars(base or Phy()) | raw
    for key in ("implicit_header", "crc"):
        _boolean(values[key], f"phy.{key}")
    for key, low, high in (("frequency_hz", 1, 2_000_000_000), ("sf", 5, 12),
                           ("coding_rate", 5, 8), ("preamble", 4, 65535), ("sync_word", 0, 65535)):
        _integer(values[key], f"phy.{key}", low, high)
    if values["bandwidth_hz"] not in (62500, 125000, 250000, 500000) or type(values["bandwidth_hz"]) is not int:
        raise ValueError("phy.bandwidth_hz must be 62500, 125000, 250000 or 500000")
    return Phy(**values)


def _link(raw, base):
    _keys(raw, ("decode", "sense", "interfere", "power_dbm"), "link")
    values = vars(base) | raw
    for key in ("decode", "sense", "interfere"):
        _boolean(values[key], f"link.{key}")
    if values["power_dbm"] is not None:
        _number(values["power_dbm"], "link.power_dbm", -300, 100)
    return Link(**values)


class World:
    def __init__(self, config, env_seed=1, layout_seed=1):
        _keys(config, ("schema", "name", "description", "duration_s", "warmup_s", "utc0_ms", "nodes",
                       "links", "default_link", "noise", "outages", "actions", "zones", "radio", "roles",
                       "decisions", "origin", "max_channel_update_s", "propagation", "fading"), "scenario")
        self.env_seed = _integer(env_seed, "env_seed", 0, 2**64 - 1)
        self.layout_seed = _integer(layout_seed, "layout_seed", 0, 2**64 - 1)
        self.config = copy.deepcopy(config)
        if type(self.config.get("schema")) is not int or self.config["schema"] != 1:
            raise ValueError("scenario.schema must be 1")
        _name(self.config.get("name"), "scenario.name")
        duration = _number(self.config.get("duration_s"), "duration_s", 0, (2**63 - 1) / NS)
        if duration <= 0 or _ns(duration) <= 0:
            raise ValueError("duration_s must be positive")
        self.duration_ns = _ns(duration)
        origin = self.config.setdefault("origin", {"lat_deg": 47.1410, "lon_deg": 9.5215})
        _keys(origin, ("lat_deg", "lon_deg"), "origin")
        self.origin_lat = _number(origin.get("lat_deg"), "origin.lat_deg", -85, 85)
        self.origin_lon = _number(origin.get("lon_deg"), "origin.lon_deg", -180, 180)
        warmup = _number(self.config.setdefault("warmup_s", 0), "warmup_s", 0, duration)
        self.warmup_ns = _ns(warmup)
        if self.warmup_ns >= self.duration_ns:
            raise ValueError("warmup must be shorter than duration")
        # The portable C core uses signed UTC microseconds, including future slot deadlines.
        # Leave room for the maximum allowed drift, a serial-delay/lag mismatch and two frames.
        headroom_ms = (self.duration_ns * 101 + 100_000_000 - 1) // 100_000_000 + 3_720_000
        self.utc0_ms = _integer(self.config.setdefault("utc0_ms", 1792670400000), "utc0_ms", 0,
                                ((1 << 63) - 1) // 1000 - headroom_ms)
        for key in ("radio", "roles"):
            if key in self.config and not isinstance(self.config[key], dict):
                raise ValueError(f"{key} must be an object")
        self.config.setdefault("radio", {})
        self.nodes = {}
        self._paths = {}
        groups = {}
        node_list = _list(self.config.get("nodes"), "nodes")
        if not node_list:
            raise ValueError("nodes must not be empty")
        for node in node_list:
            _keys(node, ("id", "group", "slot", "participant", "candidate", "interval_ms", "trajectory", "clock",
                         "gnss", "phy", "roles", "duty_permille", "listen_one_in", "relay_records", "relay_ttl_s",
                         "tx_power_dbm", "beat", "move_m"), "node")
            name = _name(node.get("id"), "node.id")
            if name == "all":
                raise ValueError("node id 'all' is reserved for outage targeting")
            if name in self.nodes:
                raise ValueError(f"duplicate node {name}")
            group = _name(node.get("group"), f"{name}.group")
            _name(node.setdefault("participant", name), f"{name}.participant")
            slot = _integer(node.get("slot"), f"{name}.slot", 0, 31)
            groups.setdefault(group, []).append(slot)
            candidate = node.setdefault("candidate", "v3")
            if candidate not in ("v3", "relay", "v3-cad", "relay-cad"):
                raise ValueError(f"{name}: unsupported candidate {candidate}")
            _integer(node.setdefault("interval_ms", 4000), f"{name}.interval_ms", 100, 60000)
            _integer(node.setdefault("duty_permille", 100), f"{name}.duty_permille", 1, 1000)
            _integer(node.setdefault("listen_one_in", 32), f"{name}.listen_one_in", 0, 65535)
            _integer(node.setdefault("relay_records", 2), f"{name}.relay_records", 0, 3)
            # the send rule: a heartbeat every 2^beat frames (0: every frame) and a move of move_m
            # metres; a board's own are beat 3 and 10 m (setups/board.json)
            _integer(node.setdefault("beat", 0), f"{name}.beat", 0, 7)
            _integer(node.setdefault("move_m", 10), f"{name}.move_m", 1, 1000)
            if _number(node.setdefault("relay_ttl_s", 30), f"{name}.relay_ttl_s", 0, 86400) <= 0:
                raise ValueError("relay_ttl_s must be positive")
            _number(node.setdefault("tx_power_dbm", 14), f"{name}.tx_power_dbm", -20, 30)
            roles = _list(node.setdefault("roles", []), f"{name}.roles")
            if any(role not in ("leader", "catcher") for role in roles) or len(set(roles)) != len(roles):
                raise ValueError("roles must be distinct leader/catcher values")
            node["phy"] = vars(_phy(node.setdefault("phy", {})))
            clock = node.setdefault("clock", {})
            _keys(clock, ("drift_ppm", "offset_us", "resolution_us", "profile"), f"{name}.clock")
            _number(clock.setdefault("drift_ppm", 0), "clock.drift_ppm", -10000, 10000)
            _integer(clock.setdefault("offset_us", 1000000), "clock.offset_us", 0, 10**12)
            _integer(clock.setdefault("resolution_us", 1), "clock.resolution_us", 1, 1000000)
            gnss = node.setdefault("gnss", {})
            _keys(gnss, ("period_s", "lag_ms", "jitter_ms", "position_error_m", "nmea_only", "nmea_told_lag_ms",
                          "nmea_error_ms"), f"{name}.gnss")
            _number(gnss.setdefault("period_s", 1), "gnss.period_s", 0.001, 3600)
            _number(gnss.setdefault("lag_ms", 100), "gnss.lag_ms", 0, 3600000)
            _number(gnss.setdefault("jitter_ms", 0), "gnss.jitter_ms", 0, gnss["lag_ms"])
            _number(gnss.setdefault("position_error_m", 0), "gnss.position_error_m", 0, 100000)
            _boolean(gnss.setdefault("nmea_only", False), "gnss.nmea_only")
            _number(gnss.setdefault("nmea_told_lag_ms", 0), "gnss.nmea_told_lag_ms", 0, 2147483)
            _number(gnss.setdefault("nmea_error_ms", 100), "gnss.nmea_error_ms", 0, 2147483)
            path = _list(node.get("trajectory"), f"{name}.trajectory")
            if not path:
                raise ValueError(f"{name}: trajectory must not be empty")
            points = []
            for point in path:
                _keys(point, ("t_s", "x_m", "y_m", "z_m"), "trajectory point")
                t = _ns(_number(point.get("t_s"), "trajectory.t_s", 0, duration))
                xyz = tuple(float(_number(point.get(key, 0) if key == "z_m" else point.get(key), key))
                            for key in ("x_m", "y_m", "z_m"))
                if points and t <= points[-1][0]:
                    raise ValueError("trajectory times must strictly increase")
                points.append((t, xyz))
            if points[0][0] != 0:
                raise ValueError("trajectory must start at time zero")
            self._paths[name] = points
            self.nodes[name] = node
        self.node_ids = tuple(self.nodes)
        self._boot_ns = dict.fromkeys(self.node_ids, 0)
        self._clock_profiles = {}
        for node, cfg in self.nodes.items():
            profile = _list(cfg["clock"].get("profile", [{"from_s": 0, "drift_ppm": cfg["clock"]["drift_ppm"]}]), "clock.profile")
            if not profile:
                raise ValueError("clock.profile must not be empty")
            times, rates, integrals = [], [], []
            for entry in profile:
                _keys(entry, ("from_s", "drift_ppm"), "clock.profile entry")
                at = _ns(_number(entry.get("from_s"), "clock.profile.from_s", 0, duration))
                drift = _number(entry.get("drift_ppm"), "clock.profile.drift_ppm", -10000, 10000)
                if (not times and at != 0) or (times and at <= times[-1]):
                    raise ValueError("clock.profile must start at zero and strictly increase")
                integral = integrals[-1] + (at - times[-1]) * rates[-1] if times else Fraction(0)
                times.append(at)
                rates.append(Fraction(1) + Fraction(str(drift)) / 1_000_000)
                integrals.append(integral)
            self._clock_profiles[node] = tuple(times), tuple(rates), tuple(integrals)
        self.groups = groups
        for group, slots in groups.items():
            if sorted(slots) != list(range(len(slots))) or len(slots) > 32:
                raise ValueError(f"group {group}: slots must uniquely cover 0..size-1, with at most 32 nodes")
        participants = [(n["group"], n["participant"]) for n in self.nodes.values()]
        if len(set(participants)) != len(participants):
            raise ValueError("participants must be unique within a group")
        propagation = self.config.get("propagation")
        if propagation is not None:
            _keys(propagation, ("reference_distance_m", "reference_power_dbm", "path_loss_exponent", "min_distance_m"), "propagation")
            for field in ("reference_distance_m", "min_distance_m", "path_loss_exponent"):
                if _number(propagation.get(field), f"propagation.{field}", 0) <= 0:
                    raise ValueError(f"propagation.{field} must be positive")
            _number(propagation.get("reference_power_dbm"), "propagation.reference_power_dbm", -300, 100)
            if "max_channel_update_s" not in self.config:
                raise ValueError("propagation requires max_channel_update_s")
        self._propagation = propagation
        default = Link() if propagation is not None else Link(False, False, False, None)
        self._default_link = _link(self.config.get("default_link", {}), default)
        self._links = []
        for item in _list(self.config.setdefault("links", []), "links"):
            _keys(item, ("from", "to", "decode", "sense", "interfere", "power_dbm", "from_s", "until_s"), "link")
            source, target = self._node(item.get("from")), self._node(item.get("to"))
            if source == target:
                raise ValueError("self links are not allowed")
            start, stop = self._interval(item, duration)
            channel = _link({k: item[k] for k in ("decode", "sense", "interfere", "power_dbm") if k in item}, Link())
            self._links.append((source, target, start, stop, channel))
        noise = self.config.setdefault("noise", {})
        _keys(noise, ("background_dbm", "bursts", "processes"), "noise")
        self._background = _number(noise.setdefault("background_dbm", -120), "noise.background_dbm", -300, 100)
        self._bursts = []
        for burst in _list(noise.setdefault("bursts", []), "noise.bursts"):
            _keys(burst, ("from_s", "until_s", "receivers", "power_dbm"), "noise burst")
            start, stop = self._interval(burst, duration, require=True)
            targets = self._targets(burst.get("receivers", list(self.node_ids)))
            power = _number(burst.get("power_dbm"), "burst.power_dbm", -300, 100)
            self._bursts.append((start, stop, targets, power))
        processes = _list(noise.setdefault("processes", []), "noise.processes")
        if len(processes) > NOISE_PROCESS_LIMIT:
            raise ValueError(f"noise.processes is limited to {NOISE_PROCESS_LIMIT} processes")
        parsed = []
        expected_changes = 0
        for process in processes:
            _keys(process, ("id", "from_s", "until_s", "mean_on_s", "mean_off_s", "power_dbm", "receivers"), "noise process")
            ident = _name(process.get("id"), "noise process.id")
            start, stop = self._interval(process, duration, require=True)
            mean_on = _number(process.get("mean_on_s"), "noise process.mean_on_s", 1 / NS)
            mean_off = _number(process.get("mean_off_s"), "noise process.mean_off_s", 1 / NS)
            power = _number(process.get("power_dbm"), "noise process.power_dbm", -300, 100)
            targets = self._targets(process.get("receivers", list(self.node_ids)))
            if not targets:
                raise ValueError("noise process.receivers must not be empty")
            expected_changes += 2 * ((stop - start) / NS) / (mean_on + mean_off)
            parsed.append((ident, start, stop, mean_on, mean_off, targets, power))
        if len({entry[0] for entry in parsed}) != len(parsed):
            raise ValueError("noise process ids must be unique")
        if expected_changes > NOISE_TRANSITION_LIMIT:
            raise ValueError("expected noise transitions exceed the resource limit")
        self.noise_process_intervals = {}
        self._noise_fields = []
        self._noise_boundaries = set()
        changes = 0
        for ident, start, stop, mean_on, mean_off, targets, power in parsed:
            self._noise_boundaries.update((start, stop))
            at, on, serial, intervals = start, False, 0, []
            while at < stop:
                draw = self._draw(f"noise-process:{ident}", "on" if on else "off", serial)
                draw = min(max(draw, 2**-64), math.nextafter(1.0, 0.0))
                wait_s = -math.log1p(-draw) * (mean_on if on else mean_off)
                end = stop if wait_s >= (stop - at) / NS else min(stop, at + max(1, math.ceil(wait_s * NS)))
                if on:
                    intervals.append((at, end))
                if end == stop:
                    break
                changes += 1
                if changes > NOISE_TRANSITION_LIMIT:
                    raise ValueError("realized noise transitions exceed the resource limit")
                self._noise_boundaries.add(end)
                at, on, serial = end, not on, serial + 1
            intervals = tuple(intervals)
            self.noise_process_intervals[ident] = intervals
            self._noise_fields.append((targets, power, tuple(a for a, _ in intervals), tuple(b for _, b in intervals)))
        self._outages = []
        for outage in _list(self.config.setdefault("outages", []), "outages"):
            _keys(outage, ("node", "kind", "from_s", "until_s"), "outage")
            target = outage.get("node")
            if target != "all":
                self._node(target)
            if outage.get("kind") not in ("fix", "gnss"):
                raise ValueError("outage kind must be fix or gnss")
            start, stop = self._interval(outage, duration, require=True)
            self._outages.append((target, outage["kind"], start, stop))
        for index, action in enumerate(_list(self.config.setdefault("actions", []), "actions")):
            _keys(action, ("id", "t_s", "node", "kind", "state", "phy"), "action")
            node = self.nodes[self._node(action.get("node"))]
            _number(action.get("t_s"), "action.t_s", 0, duration)
            _name(action.setdefault("id", f"action-{index}"), "action.id")
            if action.get("kind") == "state":
                if action.get("state") not in ("normal", "help") or "phy" in action:
                    raise ValueError("implemented state actions support normal/help only")
            elif action.get("kind") == "reboot":
                if "phy" in action or "state" in action:
                    raise ValueError("reboot has no phy or state parameter")
            elif action.get("kind") == "mode":
                if "state" in action:
                    raise ValueError("mode has no state parameter")
                action["phy"] = vars(_phy(action.get("phy"), _phy(node["phy"])))
            else:
                raise ValueError("action kind must be state, reboot or mode")
        if len({a["id"] for a in self.config["actions"]}) != len(self.config["actions"]):
            raise ValueError("action ids must be unique")
        self._zones = []
        for zone in _list(self.config.setdefault("zones", []), "zones"):
            _keys(zone, ("id", "lower_m", "upper_m", "endpoint", "senders", "receivers", "link", "from_s", "until_s"), "zone")
            _name(zone.get("id"), "zone.id")
            lower, upper = _list(zone.get("lower_m"), "zone.lower_m"), _list(zone.get("upper_m"), "zone.upper_m")
            if len(lower) != 2 or len(upper) != 2:
                raise ValueError("zones require 2D rectangular lower_m/upper_m")
            lower = tuple(_number(v, "zone.lower_m") for v in lower)
            upper = tuple(_number(v, "zone.upper_m") for v in upper)
            if any(a >= b for a, b in zip(lower, upper)):
                raise ValueError("zone lower bounds must be below upper bounds")
            endpoint = zone.setdefault("endpoint", "either")
            if endpoint not in ("sender", "receiver", "either"):
                raise ValueError("zone.endpoint must be sender, receiver or either")
            senders = self._targets(zone.get("senders", list(self.node_ids)))
            receivers = self._targets(zone.get("receivers", list(self.node_ids)))
            override = zone.get("link")
            _link(override, Link())
            start, stop = self._interval(zone, duration)
            self._zones.append((lower, upper, endpoint, senders, receivers, override, start, stop))
        if len({z["id"] for z in self.config["zones"]}) != len(self.config["zones"]):
            raise ValueError("zone ids must be unique")
        update = self.config.get("max_channel_update_s")
        self._update_ns = None if update is None else _ns(_number(update, "max_channel_update_s", 1e-6, duration))
        self._fading = self.config.get("fading")
        self._fading_cache = {}
        self._fading_ns = None
        if self._fading is not None:
            fading = self._fading
            _keys(fading, ("grid_s", "std_db", "correlation_s", "scope"), "fading")
            self._fading_ns = _ns(_number(fading.get("grid_s"), "fading.grid_s", 1e-6, duration))
            _number(fading.get("std_db"), "fading.std_db", 0, 100)
            if _number(fading.get("correlation_s"), "fading.correlation_s", 0) <= 0:
                raise ValueError("fading.correlation_s must be positive")
            if fading.get("scope") not in ("link", "receiver", "global"):
                raise ValueError("fading.scope must be link, receiver or global")
            if self._propagation is None:
                channels = [self._default_link, *(item[4] for item in self._links)]
                if any((link.decode or link.sense or link.interfere) and link.power_dbm is None for link in channels):
                    raise ValueError("fading requires declared powers or propagation")
                if any("power_dbm" in zone[5] and zone[5]["power_dbm"] is None for zone in self._zones):
                    raise ValueError("fading zones must not erase declared powers")
        for interval in (self._update_ns, self._fading_ns):
            if interval and self.duration_ns // interval > 1_000_000:
                raise ValueError("channel grid exceeds one million boundary events")
        for decision in _list(self.config.setdefault("decisions", []), "decisions"):
            _keys(decision, ("id", "node", "at_s", "watch", "kind", "after_s"), "decision")
            _name(decision.get("id"), "decision.id")
            self._node(decision.get("node"))
            _number(decision.get("at_s"), "decision.at_s", 0, duration)
            self._targets(decision.get("watch", []))
            if "after_s" in decision:
                _number(decision["after_s"], "decision.after_s", 0, decision["at_s"])
            if "kind" in decision and decision["kind"] not in ("fork", "observation"):
                raise ValueError("decision.kind must be fork or observation")
        if len({d["id"] for d in self.config["decisions"]}) != len(self.config["decisions"]):
            raise ValueError("decision ids must be unique")

    def _node(self, value):
        if not isinstance(value, str) or value not in self.nodes:
            raise ValueError(f"unknown node {value!r}")
        return value

    def _targets(self, values):
        targets = tuple(self._node(v) for v in _list(values, "node list"))
        if len(set(targets)) != len(targets):
            raise ValueError("node lists must not contain duplicates")
        return frozenset(targets)

    @staticmethod
    def _interval(item, duration, require=False):
        start = _number(item.get("from_s") if require else item.get("from_s", 0), "from_s", 0, duration)
        stop = _number(item.get("until_s") if require else item.get("until_s", duration), "until_s", 0, duration)
        if start >= stop or _ns(start) >= _ns(stop):
            raise ValueError("interval must have positive duration")
        return _ns(start), _ns(stop)

    def phy(self, node):
        return Phy(**self.nodes[node]["phy"])

    def geographic(self, position):
        x, y = position[:2]
        return (self.origin_lat + y / 111320,
                self.origin_lon + x / (111320 * math.cos(math.radians(self.origin_lat))))

    def metric(self, lat_deg, lon_deg):
        return ((lon_deg - self.origin_lon) * 111320 * math.cos(math.radians(self.origin_lat)),
                (lat_deg - self.origin_lat) * 111320, 0.0)

    def _segment(self, node, t_ns):
        points = self._paths[node]
        index = bisect.bisect_right([p[0] for p in points], t_ns) - 1
        return points, max(0, index)

    def position(self, node, t_ns):
        points, index = self._segment(node, t_ns)
        start, xyz = points[index]
        if index == len(points) - 1 or t_ns <= start:
            return xyz
        stop, other = points[index + 1]
        fraction = (t_ns - start) / (stop - start)
        return tuple(a + fraction * (b - a) for a, b in zip(xyz, other))

    def velocity(self, node, t_ns):
        points, index = self._segment(node, t_ns)
        if index == len(points) - 1 or t_ns < points[0][0]:
            return (0.0, 0.0, 0.0)
        start, xyz = points[index]
        stop, other = points[index + 1]
        return tuple((b - a) * NS / (stop - start) for a, b in zip(xyz, other))

    def clock_rate(self, node, at_ns=0):
        times, rates, _ = self._clock_profiles[node]
        return float(rates[max(0, bisect.bisect_right(times, at_ns) - 1)])

    def _clock_integral(self, node, at_ns):
        times, rates, integrals = self._clock_profiles[node]
        index = max(0, bisect.bisect_right(times, at_ns) - 1)
        return integrals[index] + (at_ns - times[index]) * rates[index]

    def local_us(self, node, t_ns):
        clock = self.nodes[node]["clock"]
        resolution = clock["resolution_us"]
        _, rates, _ = self._clock_profiles[node]
        if len(rates) == 1:
            rate = rates[0]
            numerator = clock["offset_us"] * US * rate.denominator + (t_ns - self._boot_ns[node]) * rate.numerator
            return numerator // (US * rate.denominator * resolution) * resolution
        elapsed = self._clock_integral(node, t_ns) - self._clock_integral(node, self._boot_ns[node])
        return (clock["offset_us"] * US + elapsed) // (US * resolution) * resolution

    def reboot(self, node, t_ns):
        self._node(node)
        _integer(t_ns, "reboot time", 0, self.duration_ns)
        self._boot_ns[node] = t_ns

    def world_deadline(self, node, local_us):
        clock = self.nodes[node]["clock"]
        resolution = clock["resolution_us"]
        _integer(local_us, "local deadline", -2**63, 2**63 - 1)
        target = -(-local_us // resolution) * resolution
        times, rates, integrals = self._clock_profiles[node]
        if len(rates) == 1:
            rate = rates[0]
            numerator = (target - clock["offset_us"]) * US * rate.denominator
            return self._boot_ns[node] - (-numerator // rate.numerator)
        integral = self._clock_integral(node, self._boot_ns[node]) + (target - clock["offset_us"]) * US
        index = max(0, bisect.bisect_right(integrals, integral) - 1)
        elapsed = (integral - integrals[index]) / rates[index]
        return times[index] - (-elapsed.numerator // elapsed.denominator)

    def link(self, source, receiver, t_ns):
        self._node(source)
        self._node(receiver)
        if source == receiver:
            return Link(False, False, False, None)
        result = self._default_link
        for a, b, start, stop, channel in self._links:
            if a == source and b == receiver and start <= t_ns < stop:
                result = channel
        for lower, upper, endpoint, senders, receivers, override, start, stop in self._zones:
            if source not in senders or receiver not in receivers or not start <= t_ns < stop:
                continue
            def inside(node):
                point = self.position(node, t_ns)
                return all(a <= p < b for a, b, p in zip(lower, upper, point))
            affected = inside(source) if endpoint == "sender" else inside(receiver) if endpoint == "receiver" else inside(source) or inside(receiver)
            if affected:
                result = _link(override, result)
        power = result.power_dbm
        if power is None and self._propagation is not None and (result.decode or result.sense or result.interfere):
            p = self._propagation
            sampled = t_ns // self._update_ns * self._update_ns
            distance = max(p["min_distance_m"], math.dist(self.position(source, sampled), self.position(receiver, sampled)))
            power = p["reference_power_dbm"] - 10 * p["path_loss_exponent"] * math.log10(distance / p["reference_distance_m"])
        if power is not None:
            power += self._fade(source, receiver, t_ns)
            result = Link(result.decode, result.sense, result.interfere, power)
        return result

    def _fade(self, source, receiver, t_ns):
        if self._fading is None:
            return 0
        cfg = self._fading
        key = "global" if cfg["scope"] == "global" else receiver if cfg["scope"] == "receiver" else json.dumps([source, receiver], separators=(",", ":"))
        cell = max(0, t_ns // self._fading_ns)
        series = self._fading_cache.setdefault(key, [])
        a = math.exp(-(self._fading_ns / NS) / cfg["correlation_s"])
        while len(series) <= cell:
            n = len(series)
            u = max(2**-64, self._draw(key, "fading-normal-u", n))
            v = self._draw(key, "fading-normal-v", n)
            normal = math.sqrt(-2 * math.log(u)) * math.cos(2 * math.pi * v)
            series.append(cfg["std_db"] * normal if not n else a * series[-1] + cfg["std_db"] * math.sqrt(1 - a * a) * normal)
        return series[cell]

    def noise_dbm(self, receiver, t_ns):
        self._node(receiver)
        powers = [self._background]
        powers.extend(power for start, stop, targets, power in self._bursts if receiver in targets and start <= t_ns < stop)
        for targets, power, starts, stops in self._noise_fields:
            if receiver not in targets:
                continue
            index = bisect.bisect_right(starts, t_ns) - 1
            if index >= 0 and t_ns < stops[index]:
                powers.append(power)
        maximum = max(powers)
        return maximum + 10 * math.log10(math.fsum(10 ** ((power - maximum) / 10) for power in powers))

    def boundaries(self):
        times = {0, self.duration_ns}
        times.update(self._noise_boundaries)
        for points in self._paths.values():
            times.update(t for t, _ in points)
        for changes, _, _ in self._clock_profiles.values():
            times.update(changes)
        for _, _, start, stop, _ in self._links:
            times.update((start, stop))
        for start, stop, _, _ in self._bursts:
            times.update((start, stop))
        for _, _, start, stop in self._outages:
            times.update((start, stop))
        times.update(_ns(action["t_s"]) for action in self.config["actions"])
        for lower, upper, _, _, _, _, start, stop in self._zones:
            times.update((start, stop))
            for points in self._paths.values():
                for (a, p), (b, q) in zip(points, points[1:]):
                    for axis in range(2):
                        if p[axis] == q[axis]:
                            continue
                        for edge in (lower[axis], upper[axis]):
                            f = ((Fraction(str(edge)) - Fraction(str(p[axis]))) /
                                 (Fraction(str(q[axis])) - Fraction(str(p[axis]))))
                            if 0 <= f <= 1:
                                t = a + (b - a) * f.numerator // f.denominator
                                # Half-open zones can change classification just after an exact crossing.
                                for boundary in (t, t + 1):
                                    if start <= boundary <= stop and 0 <= boundary <= self.duration_ns:
                                        times.add(boundary)
        for interval in (self._update_ns, self._fading_ns):
            if interval:
                times.update(range(0, self.duration_ns + 1, interval))
        return tuple(sorted(times))

    def _draw(self, node, purpose, measured_ns):
        key = json.dumps([self.env_seed, node, purpose, measured_ns], separators=(",", ":")).encode()
        return int.from_bytes(hashlib.blake2b(key, digest_size=8).digest(), "big") / 2**64

    def gnss(self, node, sequence, measurement_ns):
        settings = self.nodes[node]["gnss"]
        kinds = {kind for target, kind, start, stop in self._outages
                 if target in (node, "all") and start <= measurement_ns < stop}
        present = "gnss" not in kinds
        valid = present and "fix" not in kinds
        point = self.position(node, measurement_ns)
        radius = settings["position_error_m"] * math.sqrt(self._draw(node, "position-radius", measurement_ns))
        angle = 2 * math.pi * self._draw(node, "position-angle", measurement_ns)
        measured = (point[0] + radius * math.cos(angle), point[1] + radius * math.sin(angle), point[2])
        lag_ms = settings["lag_ms"] + settings["jitter_ms"] * (2 * self._draw(node, "nmea-jitter", measurement_ns) - 1)
        velocity = self.velocity(node, measurement_ns)
        speed = math.hypot(velocity[0], velocity[1])
        course = math.degrees(math.atan2(velocity[0], velocity[1])) % 360
        return {"id": f"{node}:fix:{measurement_ns}", "sequence": sequence, "node": node,
                "measured_ns": measurement_ns, "available_ns": measurement_ns + round(lag_ms * 1e6),
                "position": measured, "utc_ms": self.utc0_ms + measurement_ns // 1_000_000,
                "valid": valid, "time_valid": present, "present": present,
                "pps": present and not settings["nmea_only"], "speed_kmh10": round(speed * 36),
                "course_deg10": round(course * 10) % 3600}

    def pps_available(self, node, at_ns):
        return not self.nodes[node]["gnss"]["nmea_only"] and not any(
            target in (node, "all") and kind == "gnss" and start <= at_ns < stop
            for target, kind, start, stop in self._outages)
