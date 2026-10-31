"""Compose the independent world/radio models with real C protocol adapters."""
from collections import Counter, defaultdict, deque
from dataclasses import asdict
import hashlib
import json
import math

from .kernel import Engine, WORLD, SENSOR, OBSERVE, DECIDE, ACQUIRE
from .metrics import Evaluator, contact_reference
from .native import NativeLibrary, NativeNode
from .radio import RadioNetwork
from .types import NS, Phy, Record
from .world import World


def keyed(seed, *parts):
    value = json.dumps([seed, *parts], separators=(",", ":"), ensure_ascii=True).encode()
    return hashlib.sha256(value).digest()


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()


class Simulation:
    def __init__(self, config, *, environment_seed=17, layout_seed=17, protocol_seed=17,
                 reverse_ties=False, library=None, positions_period_ns=NS):
        self.world = World(config, environment_seed, layout_seed)
        self.engine = Engine(reverse_ties=reverse_ties)
        self.events, self.nodes = [], {}
        self.library = library or NativeLibrary()
        self.protocol_seed = protocol_seed
        self.layout_seed = layout_seed
        self.boots = dict.fromkeys(self.world.node_ids, 0)
        self.held = dict.fromkeys(self.world.node_ids)
        self.state_events = {}
        self.wake_handles = {}
        self.rx_senders = {}
        self.commanded_slot = {}
        self.records = {}
        self.last_applied = {}
        self.role_results = []
        self.tx_cost = defaultdict(list)
        self.tx_signals = {}
        self.contract_failures = []
        groups = {node: cfg["group"] for node, cfg in self.world.nodes.items()}
        self.members = {group: {cfg["slot"]: node for node, cfg in self.world.nodes.items() if cfg["group"] == group}
                        for group in set(groups.values())}
        self.evaluator = Evaluator(groups, self.world.duration_ns, self.world.warmup_ns,
                                   self.world.position, utc0_ns=self.world.utc0_ms * 1_000_000)
        radio_config = self.world.config.get("radio", {}) | {"seed": environment_seed}
        self.radio = RadioNetwork(self.engine, self.world, self._observe, self._trace, radio_config)
        self.radio.tx_started = self._rf_started
        try:
            for node in self.world.node_ids:
                self._create(node)
                self.radio.add_node(node, self.world.phy(node), self.world.nodes[node]["tx_power_dbm"])
                self._state(node, "normal", f"{node}:boot:0")
                self.engine.schedule(0, SENSOR, self._sensor, node, 0)
                first_edge = (-self.world.utc0_ms * 1_000_000) % NS
                self.engine.schedule(first_edge, SENSOR, self._pps_tick, node)
                for point in self.world.nodes[node]["clock"].get("profile", []):
                    self.engine.schedule(round(point["from_s"] * NS), WORLD, self._clock_changed, node)
            for at in self.world.boundaries():
                if at < self.world.duration_ns:
                    self.engine.schedule(at, WORLD, self.radio.environment_changed)
            for action in self.world.config["actions"]:
                self.engine.schedule(round(action["t_s"] * NS), WORLD, self._action, action)
            for decision in self.world.config.get("decisions", []):
                self.engine.schedule(round(decision["at_s"] * NS), ACQUIRE, self._decision, decision)
            for at in range(0, self.world.duration_ns + 1, positions_period_ns):
                self.engine.schedule(at, ACQUIRE, self._positions)
        except Exception:
            self.close()
            raise

    def _create(self, node):
        cfg = self.world.nodes[node]
        key = keyed(self.layout_seed, "group-key", cfg["group"])[:16]
        seed = int.from_bytes(keyed(self.protocol_seed, "node", node, self.boots[node])[:4], "big") or 1
        self.nodes[node] = NativeNode(self.library, cfg, len(self.members[cfg["group"]]), key, seed)

    def _emit(self, event, **values):
        self.events.append({"event": event, "t_ns": self.engine.now_ns, **values})

    def _failure(self, check, **values):
        item = {"check": check, "t_ns": self.engine.now_ns, **values}
        self.contract_failures.append(item)
        self._emit("contract_failure", **item)

    def _trace(self, event):
        self.evaluator.advance(self.engine.now_ns)
        if event["event"] == "tx":
            node = event["sender"]
            event["records"] = [self._record_key(node, record) for record in
                                 self.nodes[node].records(bytes.fromhex(event["payload_hex"]))]
            self.tx_signals[event["tx_id"]] = event
        elif event["event"] == "tx_end":
            tx = self.tx_signals[event["tx_id"]]
            self.tx_cost[event["sender"]].append((tx["start_ns"], event["t_ns"]))
        elif event["event"] == "rx" and event["accepted"]:
            self.rx_senders[event["receiver"], event["t_ns"], event["payload_hex"]] = event["sender"]
            self.evaluator.radio_received(event["receiver"], event["t_ns"], event["sender"])
        self.events.append(event)

    def _record_key(self, receiver, value):
        group = self.world.nodes[receiver]["group"]
        return f"{group}/{value['slot']}/{value['session']:08x}/{value['counter']}"

    def _position(self, value):
        lat, lon = value["lat_e7"] / 1e7, value["lon_e7"] / 1e7
        return ((lon - 9.5215) * 111320 * math.cos(math.radians(47.1410)),
                (lat - 47.1410) * 111320, 0.0)

    def _register(self, node, payload):
        cfg = self.world.nodes[node]
        self.evaluator.advance(self.engine.now_ns)
        for value in self.nodes[node].records(payload):
            if value["slot"] != cfg["slot"]:
                continue
            key = self._record_key(node, value)
            record = Record(key, node, self.boots[node], self.engine.now_ns, self.held[node],
                            self.state_events[node], self._position(value), "help" if value["status"] & 1 else "normal")
            if key in self.records and self.records[key] != record:
                self._failure("native_source_identity_collision", node=node, record_key=key)
                continue
            self.records[key] = record
            self.evaluator.source_record(record)
            self._emit("source_record", record=asdict(record))

    def _calendar(self, node):
        now = self.engine.now_ns
        reading = self.nodes[node].clock(self.world.local_us(node, now))
        self.evaluator.calendar(node, now, None if reading is None else reading[0] * 1000,
                                self.world.clock_rate(node, now))

    def _clock_changed(self, node):
        self._calendar(node)
        self._schedule_wake(node)

    def _schedule_wake(self, node):
        local = self.nodes[node].next(self.world.local_us(node, self.engine.now_ns))
        old = self.wake_handles.pop(node, None)
        if old is not None:
            old.cancel()
        if local is None:
            return
        at = max(self.engine.now_ns, self.world.world_deadline(node, local))
        if at < self.world.duration_ns:
            self.wake_handles[node] = self.engine.schedule(at, DECIDE, self._wake, node, self.boots[node])

    def _wake(self, node, boot):
        if self.boots[node] != boot or self.engine.now_ns >= self.world.duration_ns:
            return
        self.wake_handles.pop(node, None)
        native = self.nodes[node]
        payload = native.poll(self.world.local_us(node, self.engine.now_ns))
        if payload:
            self._register(node, payload)
            self.commanded_slot[node] = native.target_utc_us() * 1000
            self.radio.transmit(node, payload)
        self._schedule_wake(node)

    def _rf_started(self, node, signal):
        reading = self.nodes[node].clock(self.world.local_us(node, signal.start_ns))
        if reading is None:
            self._failure("transmit_without_time", node=node)
            return
        estimated, uncertainty = reading
        error = estimated * 1000 - (self.world.utc0_ms * 1_000_000 + signal.start_ns)
        target = self.commanded_slot[node]
        self._emit("tx_clock", node=node, tx_id=signal.id, true_error_ns=error,
                   declared_error_ns=uncertainty * 1000, nominal_utc_ns=target,
                   start_offset_ns=self.world.utc0_ms * 1_000_000 + signal.start_ns - target)

    def _sensor(self, node, sequence):
        now = self.engine.now_ns
        if now >= self.world.duration_ns:
            return
        reading = self.world.gnss(node, sequence, now)
        self.evaluator.advance(now)
        if reading["valid"]:
            self.evaluator.measurement(reading["id"], node, now, self.world.position(node, now))
        self._emit("measurement", **reading, true_position=self.world.position(node, now))
        boot = self.boots[node]
        if reading["present"] and reading["available_ns"] < self.world.duration_ns:
            self.engine.schedule(reading["available_ns"], OBSERVE, self._fix, node, reading, boot)
        period = round(self.world.nodes[node]["gnss"]["period_s"] * NS)
        self.engine.schedule((sequence + 1) * period, SENSOR, self._sensor, node, sequence + 1)

    def _pps_tick(self, node):
        now = self.engine.now_ns
        if now >= self.world.duration_ns:
            return
        if self.world.pps_available(node, now):
            self.nodes[node].pps(self.world.local_us(node, now))
            self._emit("pps", node=node, local_us=self.world.local_us(node, now))
        self.engine.schedule(now + NS, SENSOR, self._pps_tick, node)

    def _fix(self, node, reading, boot):
        if self.boots[node] != boot:
            return
        x, y, _ = reading["position"]
        lat = round((47.1410 + y / 111320) * 1e7)
        lon = round((9.5215 + x / (111320 * math.cos(math.radians(47.1410)))) * 1e7)
        self.nodes[node].fix(self.world.local_us(node, self.engine.now_ns), int(reading["utc_ms"]),
                             reading["valid"], reading["time_valid"], lat, lon,
                             reading["speed_kmh10"], reading["course_deg10"])
        if reading["valid"]:
            self.held[node] = reading["id"]
        self._emit("sensor_delivered", node=node, measurement_id=reading["id"], valid=reading["valid"])
        self._calendar(node)
        self._schedule_wake(node)

    def _state(self, node, state, event_id):
        self.state_events[node] = event_id
        self.evaluator.state(event_id, node, state, self.engine.now_ns)
        self._emit("state", node=node, state=state, event_id=event_id)

    def _action(self, action):
        node = action["node"]
        if action["kind"] == "state":
            self.nodes[node].help(action["state"] == "help")
            self._state(node, action["state"], action["id"])
        elif action["kind"] == "mode":
            phy = Phy(**action["phy"])
            self.nodes[node].mode(phy, False)
            self.radio.configure(node, phy)
        elif action["kind"] == "reboot":
            self.radio.reboot(node)
            self.nodes[node].close()
            self.boots[node] += 1
            self.world.reboot(node, self.engine.now_ns)
            self.held[node] = None
            self.evaluator.forget(node, self.engine.now_ns)
            self.last_applied = {pair:value for pair,value in self.last_applied.items() if pair[0] != node}
            self._emit("view_reset", receiver=node)
            self._create(node)
            self._state(node, "normal", f"{node}:boot:{self.boots[node]}")
            self._calendar(node)
            self.nodes[node].mode(self.world.phy(node), False)
            self.radio.configure(node, self.world.phy(node))
        self._schedule_wake(node)

    def _observe(self, node, observation):
        now = self.engine.now_ns
        local = self.world.local_us(node, now)
        if observation.kind == "RX_DONE":
            sender = self.rx_senders.pop((node, observation.at_ns, observation.payload.hex()), None)
            slots = self.nodes[node].receive(observation.payload, local, observation.rssi_dbm,
                                             observation.snr_db, self.world.local_us(node, observation.at_ns))
            self._emit("packet_applied", receiver=node, accepted_slots=slots,
                       rssi_dbm=observation.rssi_dbm, snr_db=observation.snr_db)
            group = self.world.nodes[node]["group"]
            for slot in slots:
                value = self.nodes[node].view(slot)
                author = self.members[group][slot]
                key = self._record_key(node, value)
                position = self._position(value)
                state = "help" if value["status"] & 1 else "normal"
                self.evaluator.applied(node, key, now, value["fix_utc_ms"] * 1_000_000 or None,
                                       sender, author=author, position=position, state=state)
                self.last_applied[node, author] = key, now
                self._emit("applied", receiver=node, author=author, record_key=key, via=sender,
                           position=position, state=state, display_fix_ns=value["fix_utc_ms"] * 1_000_000 or None)
        elif observation.kind == "TX_DONE":
            self.nodes[node].tx_done(self.world.local_us(node, observation.at_ns))
        elif observation.kind == "MODE_APPLIED":
            if observation.success:
                self.nodes[node].mode(self.radio.radios[node].phy, True)
                self.radio.listen(node)
        self._emit("observation", node=node, kind=observation.kind,
                   stamped_ns=observation.at_ns, success=observation.success)
        self._calendar(node)
        self._schedule_wake(node)

    def _positions(self):
        self._emit("positions", positions={node: self.world.position(node, self.engine.now_ns)
                                           for node in self.world.node_ids})

    def _decision(self, decision):
        node = decision["node"]
        after = round(decision.get("after_s", 0) * NS)
        reset_at = max((e["t_ns"] for e in self.events
                        if e["event"] == "view_reset" and e["receiver"] == node), default=0)
        for author in decision.get("watch", []):
            pair = self.evaluator.pairs.get((node, author))
            current_fix = self.evaluator.fixes.get(pair.view.fix_id) if pair and pair.view else None
            available = current_fix is not None and current_fix["measured_ns"] >= after
            qualifying = []
            for event in self.events:
                if (event["event"] == "applied" and event["receiver"] == node and event["author"] == author
                        and event["t_ns"] >= reset_at):
                    record = self.records.get(event["record_key"])
                    fix = self.evaluator.fixes.get(record.fix_id) if record else None
                    if fix and fix["measured_ns"] >= after:
                        qualifying.append(event["t_ns"])
            first = min(qualifying) if available and qualifying else None
            self.role_results.append({"decision": decision["id"], "node": node, "author": author,
                "at_ns": self.engine.now_ns, "measurement_after_ns": after,
                "information_available": available,
                "first_qualifying_ns": first, "margin_ns": self.engine.now_ns-first if first is not None else None,
                "criterion": "applied source measurement at or after scenario's declared branch threshold"})

    def run(self, contact_diagnostic=False):
        self.engine.run(self.world.duration_ns)
        for signal in self.radio.active.values():
            self.tx_cost[signal.sender].append((signal.start_ns, self.world.duration_ns))
            self._emit("observation_cut", tx_id=signal.id, sender=signal.sender,
                       observed_airtime_ns=self.world.duration_ns-signal.start_ns, planned_end_ns=signal.end_ns)
        metrics = self.evaluator.finish()
        air = []
        for node, cfg in self.world.nodes.items():
            window, active, total, maximum = 3600 * NS, deque(), 0, 0
            for start, stop in self.tx_cost[node]:
                active.append((start, stop))
                total += stop-start
                while active and active[0][1] <= stop-window:
                    left, right = active.popleft()
                    total -= right-left
                partial = max(0, stop-window-active[0][0]) if active else 0
                maximum = max(maximum, total-partial)
            limit = window*cfg["duty_permille"]//1000
            if maximum > limit:
                self._failure("rolling_air_budget", node=node, used_ns=maximum, limit_ns=limit)
            air.append({"node": node, "airtime_ns": sum(b-a for a,b in self.tx_cost[node]),
                        "rolling_hour_max_ns": maximum, "rolling_hour_limit_ns": limit,
                        **self.nodes[node].statistics(self.world.local_us(node, self.world.duration_ns))})
        costs = Counter(e["event"] for e in self.events)
        return {"schema": 1, "evidence": "C core with Python DES, not board or phone",
                "metrics": metrics, "airtime": air, "role_decisions": self.role_results,
                "event_counts": dict(costs), "contract_failures": self.contract_failures,
                "checks_executed": ["native_source_identity", "transmit_has_time", "actual_rolling_air_budget"],
                "checks_not_exercised": ["full_slot_containment", "radio_model_hardware_agreement",
                                         "security_attack_resistance", "phone_map_delivery"],
                "radio_capabilities": self.radio.capabilities,
                "radio_parameters": self.radio.config,
                "candidate_capabilities": {"v3": {"core":"production C node and codec", "states":["normal","help"],
                    "automatic_help_answers":True, "before_first_position":False},
                    "relay":{"core":"research C framing and forwarding around v3", "selection":"least recently forwarded",
                             "cache_expiry":"since acceptance of a new authenticated source packet",
                             "source_authentication":True,"envelope_authentication":False,
                             "wire_relay_peer_identity":False,"states":["normal","help"],
                             "automatic_help_answers":True,"before_first_position":False}},
                "contact_reference": contact_reference(self.events) if contact_diagnostic else
                    {"enabled": False, "reason": "opt in with --reference; unlimited-content diagnostic can be expensive"},
                "numerical_assumptions": {"world_time_unit": "ns", "native_clock_unit": "us",
                    "displayed_calendar": "piecewise affine between observations",
                    "calendar_quantization_bound_ns": {node:cfg["clock"]["resolution_us"]*1000
                                                       for node,cfg in self.world.nodes.items()},
                    "geographic_projection": "local tangent approximation at 47.1410 N, 9.5215 E; 111320 m per degree"},
                "not_implemented": ["calibrated RF", "DEM propagation", "real phone delivery", "energy model",
                                    "Meshtastic/SDK firmware adapters"]}

    def close(self):
        for node in self.nodes.values():
            node.close()
