"""Radio lifecycle and local observations over explicit channel histories.

Acquisition/header timing and capture thresholds are scenario hypotheses. The
decoder models failed receptions as erasures, not invented corrupted payloads.
"""
from dataclasses import asdict, dataclass, field
import hashlib
import math

from .kernel import COMPLETE, OBSERVE, START, ACQUIRE
from .types import Observation, Phy


@dataclass(frozen=True)
class Signal:
    id: str
    sender: str
    payload: bytes
    phy: Phy
    start_ns: int
    end_ns: int
    power_dbm: float

    @property
    def tx_id(self):
        return self.id


@dataclass(frozen=True)
class Interval:
    start_ns: int
    end_ns: int
    decodable: bool
    desired_dbm: float | None
    noise_dbm: float
    interferers: tuple[tuple[str, float | None], ...]


@dataclass
class Reception:
    signal: Signal
    started_ns: int
    last_ns: int
    token: int
    intervals: list[Interval] = field(default_factory=list)


@dataclass
class Sensing:
    started_ns: int
    last_ns: int
    end_ns: int
    token: int
    busy: bool = False


@dataclass
class Radio:
    phy: Phy
    power_dbm: float
    state: str = "RX_SEARCH"
    epoch: int = 0
    token: int = 0
    tx_number: int = 0
    config_number: int = 0
    notification_generation: int = 0
    cad_number: int = 0
    applied: bool = True
    expected_length: int | None = None
    rx: Reception | None = None
    cad: Sensing | None = None
    tx: Signal | None = None
    failed_acquisitions: set[str] = field(default_factory=set)


def _watts(dbm):
    return 10 ** ((dbm - 30) / 10)


def _dbm(watts):
    return 10 * math.log10(watts) + 30 if watts > 0 else -math.inf


class RadioNetwork:
    def __init__(self, engine, world, observe, emit, config=None):
        self.engine, self.world = engine, world
        self.observe, self.emit = observe, emit
        allowed = {"reception_model", "acquisition_symbols", "header_symbols", "tx_prepare_ns",
                   "rx_turnaround_ns", "irq_delay_ns", "cad_symbols", "cad_false_positive",
                   "cad_false_negative", "seed", "reference_tx_power_dbm", "capture_threshold_db",
                   "noise_threshold_dbm", "cad_noise_threshold_dbm", "cad_signal_threshold_dbm",
                   "sensitivity_dbm", "trace_intervals", "implicit_payload_length"}
        if config is not None and (not isinstance(config, dict) or set(config) - allowed):
            raise ValueError("unknown radio configuration")
        self.config = {"reception_model": "conservative", "acquisition_symbols": 4,
                       "header_symbols": 1, "tx_prepare_ns": 0, "rx_turnaround_ns": 0,
                       "irq_delay_ns": 0, "cad_symbols": 2, "cad_false_positive": 0,
                       "cad_false_negative": 0, "seed": 1, "reference_tx_power_dbm": 14,
                       **(config or {})}
        self.capabilities = {"corrupted_byte_delivery": False, "receiver_recapture": False,
                             "calibrated_rf": False, "acquisition_timing": "synthetic",
                             "edge_stamp_bias": False,
                             "irq_delay_semantics": "delayed notification; original completion-edge timestamp",
                             "rssi_model": "desired_signal_time_mean",
                             "snr_model": "desired_signal_to_background"}
        if type(self.config.get("trace_intervals", False)) is not bool:
            raise ValueError("trace_intervals must be boolean")
        length = self.config.get("implicit_payload_length")
        if length is not None and (type(length) is not int or not 0 <= length <= 255):
            raise ValueError("implicit_payload_length must be an integer in 0..255")
        self.model = self.config.get("reception_model", "conservative")
        if self.model not in ("conservative", "capture", "energy"):
            raise ValueError("unknown reception model")
        if self.model != "conservative" and "capture_threshold_db" not in self.config:
            raise ValueError("power reception models need an explicit capture_threshold_db")
        for name in ("tx_prepare_ns", "rx_turnaround_ns", "irq_delay_ns", "header_symbols"):
            value = self.config.get(name, 1 if name == "header_symbols" else 0)
            if type(value) is not int or value < 0:
                raise ValueError(f"{name} must be a nonnegative integer")
        for name, default in (("acquisition_symbols", 4), ("cad_symbols", 2)):
            value = self.config.get(name, default)
            if type(value) is not int or value <= 0:
                raise ValueError(f"{name} must be a positive integer")
        for name in ("cad_false_positive", "cad_false_negative"):
            value = self.config.get(name, 0)
            if type(value) not in (int, float) or not 0 <= value <= 1:
                raise ValueError(f"{name} must be in 0..1")
        for name in ("capture_threshold_db", "noise_threshold_dbm", "cad_noise_threshold_dbm",
                     "cad_signal_threshold_dbm", "sensitivity_dbm", "reference_tx_power_dbm"):
            if name in self.config and (type(self.config[name]) not in (int, float) or
                                        not math.isfinite(self.config[name])):
                raise ValueError(f"{name} must be finite")
        if type(self.config["seed"]) is not int or not 0 <= self.config["seed"] < 2**64:
            raise ValueError("radio seed must be an unsigned 64-bit integer")
        self.radios = {}
        self.active = {}
        self.tx_started = None
        self.tx_ended = None

    def _emit(self, event, **values):
        self.emit({"event": event, "t_ns": self.engine.now_ns, **values})

    def _notify(self, node, observation):
        # shortcut: stamps use ideal edges; add ISR stamp bias when E0 latency observations exist.
        radio = self.radios[node]
        epoch = radio.epoch
        generation = radio.notification_generation
        configuration = radio.config_number
        mode_sensitive = observation.kind in ("PREAMBLE_DETECTED", "HEADER_VALID", "CAD_DONE", "RX_DONE", "RX_ERROR")
        def deliver():
            current = self.radios[node]
            if current.epoch != epoch or (mode_sensitive and current.notification_generation != generation):
                return
            if observation.kind == "MODE_APPLIED" and current.config_number != configuration:
                return
            self.observe(node, observation)
        self.engine.schedule(self.engine.now_ns + self.config.get("irq_delay_ns", 0), OBSERVE, deliver)

    def add_node(self, node_id, phy, power_dbm=14):
        if node_id in self.radios or not math.isfinite(power_dbm):
            raise ValueError("duplicate radio or invalid TX power")
        self.radios[node_id] = Radio(phy, power_dbm,
                                    expected_length=self.config.get("implicit_payload_length"))

    def state(self, node):
        return self.radios[node].state

    @staticmethod
    def _symbol(phy):
        return (1 << phy.sf) * 1_000_000_000 // phy.bandwidth_hz

    @staticmethod
    def _spectral(a, b):
        return abs(a.frequency_hz - b.frequency_hz) * 2 < a.bandwidth_hz + b.bandwidth_hz

    def _compatible(self, signal, radio):
        a, b = signal.phy, radio.phy
        return (radio.applied and a.frequency_hz == b.frequency_hz and
                a.bandwidth_hz == b.bandwidth_hz and a.sf == b.sf and
                a.sync_word == b.sync_word and a.implicit_header == b.implicit_header and
                (not b.implicit_header or
                 (a.coding_rate == b.coding_rate and a.crc == b.crc and
                  radio.expected_length == len(signal.payload))))

    def _power(self, signal, receiver, t):
        link = self.world.link(signal.sender, receiver, t)
        # Link power is specified for the scenario's reference TX power.
        power = None if link.power_dbm is None else link.power_dbm + signal.power_dbm - self.config["reference_tx_power_dbm"]
        return link, power

    def _sample(self, node, rx, start, end):
        t = (start + end) // 2
        radio = self.radios[node]
        link, power = self._power(rx.signal, node, t)
        decodable = link.decode and self._compatible(rx.signal, radio)
        sensitivity = self.config.get("sensitivity_dbm")
        if sensitivity is not None:
            if power is None:
                raise ValueError("sensitivity model requires received power")
            decodable = decodable and power >= sensitivity
        interference = []
        for signal in self.active.values():
            if signal.id == rx.signal.id or signal.sender == node or not self._spectral(signal.phy, radio.phy):
                continue
            other, received = self._power(signal, node, t)
            if other.interfere:
                interference.append((signal.id, received))
        return Interval(start, end, decodable, power, self.world.noise_dbm(node, t),
                        tuple(sorted(interference)))

    def _cad_busy(self, node, t):
        radio = self.radios[node]
        noise_limit = self.config.get("cad_noise_threshold_dbm")
        if noise_limit is not None and self.world.noise_dbm(node, t) >= noise_limit:
            return True
        for signal in self.active.values():
            # CAD is LoRa-config-specific; energy CCA can additionally use local noise.
            if (signal.sender != node and signal.phy.sf == radio.phy.sf and
                    signal.phy.bandwidth_hz == radio.phy.bandwidth_hz and
                    self._spectral(signal.phy, radio.phy) and
                    self.world.link(signal.sender, node, t).sense):
                threshold = self.config.get("cad_signal_threshold_dbm")
                _, power = self._power(signal, node, t)
                if threshold is not None and power is None:
                    raise ValueError("power-based CAD requires received power")
                if threshold is None or power >= threshold:
                    return True
        return False

    def _advance(self):
        now = self.engine.now_ns
        for node, radio in self.radios.items():
            rx = radio.rx
            if rx and rx.last_ns < now:
                interval = self._sample(node, rx, rx.last_ns, now)
                rx.intervals.append(interval)
                rx.last_ns = now
                if self.config.get("trace_intervals", False):
                    self._emit("rx_interval", receiver=node, signal_id=rx.signal.id,
                               **asdict(interval))
            cad = radio.cad
            if cad and cad.last_ns < now:
                cad.busy |= self._cad_busy(node, (cad.last_ns + now) // 2)
                cad.last_ns = now

    def environment_changed(self):
        self._advance()
        self._schedule_acquisitions()

    def _schedule_acquisitions(self):
        for node in sorted(self.radios):
            if self.radios[node].state == "RX_SEARCH":
                self.engine.schedule(self.engine.now_ns, ACQUIRE, self._try_acquire, node)

    def _try_acquire(self, node):
        radio = self.radios[node]
        if radio.state != "RX_SEARCH" or not radio.applied:
            return
        now = self.engine.now_ns
        required = self.config.get("acquisition_symbols", 4) * self._symbol(radio.phy)
        choices = []
        for signal in self.active.values():
            if signal.sender == node or signal.id in radio.failed_acquisitions or not self._compatible(signal, radio):
                continue
            link, power = self._power(signal, node, now)
            if not link.decode:
                continue
            if self.model != "conservative" and power is None:
                raise ValueError("capture/energy models require every received power")
            sensitivity = self.config.get("sensitivity_dbm")
            if sensitivity is not None and (power is None or power < sensitivity):
                continue
            preamble_end = signal.start_ns + (4 * signal.phy.preamble + 17) * self._symbol(signal.phy) // 4
            if now + required <= preamble_end and now + required < signal.end_ns:
                choices.append((-(power if power is not None else 0), signal.id, signal))
        if not choices:
            return
        signal = min(choices)[2]
        radio.token += 1
        radio.rx = Reception(signal, now, now, radio.token)
        radio.state = "RX_ACQUISITION"
        self._emit("rx_acquire", receiver=node, signal_id=signal.id, tx_id=signal.id, sender=signal.sender)
        self.engine.schedule(now + required, COMPLETE, self._acquired, node, radio.token)

    def _acquired(self, node, token):
        radio = self.radios[node]
        if radio.rx is None or radio.rx.token != token:
            return
        self._advance()
        accepted, reason = self._outcome(radio.rx)
        if not accepted:
            radio.failed_acquisitions.add(radio.rx.signal.id)
            self._finish_rx(node, "acquisition_" + reason)
            self._schedule_acquisitions()
            return
        radio.state = "RX_PREAMBLE"
        self._notify(node, Observation("PREAMBLE_DETECTED", self.engine.now_ns))
        signal = radio.rx.signal
        end = signal.start_ns + (4 * signal.phy.preamble + 17) * self._symbol(signal.phy) // 4
        self.engine.schedule(end, COMPLETE, self._header, node, token)

    def _header(self, node, token):
        radio = self.radios[node]
        if radio.rx is None or radio.rx.token != token:
            return
        self._advance()
        signal = radio.rx.signal
        radio.state = "RX_HEADER" if not signal.phy.implicit_header else "RX_PAYLOAD"
        duration = 0 if signal.phy.implicit_header else self.config.get("header_symbols", 1) * self._symbol(signal.phy)
        self.engine.schedule(min(signal.end_ns, self.engine.now_ns + duration), COMPLETE,
                             self._payload, node, token)

    def _payload(self, node, token):
        radio = self.radios[node]
        if radio.rx is None or radio.rx.token != token:
            return
        self._advance()
        accepted, reason = self._outcome(radio.rx)
        if not accepted:
            radio.failed_acquisitions.add(radio.rx.signal.id)
            self._finish_rx(node, "header_" + reason)
            self._schedule_acquisitions()
            return
        radio.state = "RX_PAYLOAD"
        if not radio.rx.signal.phy.implicit_header:
            self._notify(node, Observation("HEADER_VALID", self.engine.now_ns))

    def _outcome(self, rx):
        if not rx.intervals or any(not i.decodable for i in rx.intervals):
            return False, "link_or_phy"
        if self.model == "conservative":
            if any(i.interferers for i in rx.intervals):
                return False, "interference"
            threshold = self.config.get("noise_threshold_dbm")
            if threshold is not None and any(i.noise_dbm >= threshold for i in rx.intervals):
                return False, "external_noise"
            return True, "accepted"
        desired_energy = interference_energy = 0
        ratio = 10 ** (self.config["capture_threshold_db"] / 10)
        for interval in rx.intervals:
            if interval.desired_dbm is None or any(power is None for _, power in interval.interferers):
                raise ValueError("capture/energy models require every received power")
            desired = _watts(interval.desired_dbm)
            interference = _watts(interval.noise_dbm) + sum(_watts(p) for _, p in interval.interferers)
            if self.model == "capture" and desired < ratio * interference:
                return False, "capture_threshold"
            duration = interval.end_ns - interval.start_ns
            desired_energy += desired * duration
            interference_energy += interference * duration
        okay = desired_energy >= ratio * interference_energy
        return okay, "accepted" if okay else "interference_energy"

    def _finish_rx(self, node, reason=None, notify=True):
        radio = self.radios[node]
        rx = radio.rx
        if rx is None:
            return
        accepted, why = self._outcome(rx) if reason is None else (False, reason)
        radio.rx = None
        radio.state = "RX_SEARCH"
        duration = sum(i.end_ns - i.start_ns for i in rx.intervals)
        known_power = all(i.desired_dbm is not None for i in rx.intervals)
        rssi = (_dbm(sum(_watts(i.desired_dbm) * (i.end_ns - i.start_ns)
                         for i in rx.intervals) / duration) if known_power and duration else None)
        noise = (_dbm(sum(_watts(i.noise_dbm) * (i.end_ns - i.start_ns)
                          for i in rx.intervals) / duration) if duration else None)
        self._emit("rx", receiver=node, sender=rx.signal.sender, signal_id=rx.signal.id,
                   tx_id=rx.signal.id, accepted=accepted, decode_success=accepted,
                   reason=why, payload_hex=rx.signal.payload.hex() if accepted else None,
                   rssi_dbm=rssi, snr_db=None if rssi is None or noise is None else rssi - noise)
        if notify:
            self._notify(node, Observation("RX_DONE" if accepted else "RX_ERROR", self.engine.now_ns,
                                           rx.signal.payload if accepted else b"", rssi,
                                           None if rssi is None or noise is None else rssi - noise, accepted))

    def transmit(self, node, payload, phy=None, power_dbm=None):
        radio = self.radios[node]
        payload = bytes(payload)
        chosen = radio.phy if phy is None else phy
        power = radio.power_dbm if power_dbm is None else power_dbm
        if (not radio.applied or chosen != radio.phy or
                radio.state in ("TX", "TX_PREPARE", "CONFIGURE", "RX_TURNAROUND", "SLEEP") or
                not math.isfinite(power) or not 0 <= len(payload) <= 255):
            self._notify(node, Observation("TX_ERROR", self.engine.now_ns, success=False))
            return False
        self._advance()
        if radio.rx:
            self._finish_rx(node, "half_duplex")
        radio.token += 1
        radio.cad = None
        radio.state = "TX_PREPARE"
        self.engine.schedule(self.engine.now_ns + self.config.get("tx_prepare_ns", 0), START,
                             self._start_tx, node, radio.epoch, radio.token, payload, chosen, power)
        return True

    def _start_tx(self, node, epoch, token, payload, phy, power):
        radio = self.radios[node]
        if radio.epoch != epoch or radio.token != token or radio.state != "TX_PREPARE":
            return
        self._advance()
        now = self.engine.now_ns
        radio.tx_number += 1
        signal = Signal(f"{node}:{epoch}:{radio.tx_number}", node, payload, phy, now,
                        now + phy.airtime_ns(len(payload)), power)
        radio.tx, radio.state = signal, "TX"
        self.active[signal.id] = signal
        self._emit("tx", sender=node, signal_id=signal.id, tx_id=signal.id, start_ns=now, end_ns=signal.end_ns,
                   payload_hex=payload.hex(), bytes=len(payload), power_dbm=power, phy=asdict(phy))
        if self.tx_started:
            self.tx_started(node, signal)
        self.engine.schedule(signal.end_ns, COMPLETE, self._end_tx, node, signal.id, epoch)
        self._schedule_acquisitions()

    def _end_tx(self, node, signal_id, epoch):
        radio = self.radios[node]
        if signal_id not in self.active:
            return
        self._advance()
        signal = self.active.pop(signal_id)
        for receiver, other in self.radios.items():
            other.failed_acquisitions.discard(signal_id)
            if other.rx and other.rx.signal.id == signal_id:
                self._finish_rx(receiver)
        self._emit("tx_end", sender=node, signal_id=signal_id, tx_id=signal_id, aborted=False,
                   airtime_ns=self.engine.now_ns - signal.start_ns)
        if self.tx_ended:
            self.tx_ended(node, signal)
        if radio.epoch == epoch and radio.tx and radio.tx.id == signal_id:
            radio.tx = None
            self._notify(node, Observation("TX_DONE", self.engine.now_ns))
            self._return_to_rx(node)
        self._schedule_acquisitions()

    def _return_to_rx(self, node):
        radio = self.radios[node]
        radio.token += 1
        radio.state = "RX_TURNAROUND"
        self.engine.schedule(self.engine.now_ns + self.config.get("rx_turnaround_ns", 0), START,
                             self._listen_ready, node, radio.epoch, radio.token)

    def _listen_ready(self, node, epoch, token):
        radio = self.radios[node]
        if radio.epoch == epoch and radio.token == token and radio.applied:
            radio.state = "RX_SEARCH"
            self._schedule_acquisitions()

    def cad(self, node, duration_ns=None):
        radio = self.radios[node]
        if radio.state != "RX_SEARCH" or not radio.applied:
            return False
        duration = duration_ns if duration_ns is not None else self.config.get("cad_symbols", 2) * self._symbol(radio.phy)
        if type(duration) is not int or duration <= 0:
            raise ValueError("CAD duration must be a positive integer")
        self._advance()
        radio.token += 1
        radio.cad_number += 1
        radio.state = "CAD"
        now = self.engine.now_ns
        radio.cad = Sensing(now, now, now + duration, radio.token)
        self.engine.schedule(now + duration, COMPLETE, self._end_cad, node, radio.epoch, radio.token)
        return True

    def _draw(self, node, start, end):
        key = f"{self.config.get('seed', 1)}:cad:{node}:{start}:{end}".encode()
        return int.from_bytes(hashlib.sha256(key).digest()[:8], "big") / 2 ** 64

    def _end_cad(self, node, epoch, token):
        radio = self.radios[node]
        if radio.epoch != epoch or radio.cad is None or radio.cad.token != token:
            return
        self._advance()
        busy = radio.cad.busy
        draw = self._draw(node, radio.cad.started_ns, radio.cad.end_ns)
        if busy and draw < self.config.get("cad_false_negative", 0):
            busy = False
        elif not busy and draw < self.config.get("cad_false_positive", 0):
            busy = True
        self._emit("cad", node=node, busy=busy, start_ns=radio.cad.started_ns)
        radio.cad = None
        radio.state = "RX_SEARCH"
        self._notify(node, Observation("CAD_DONE", self.engine.now_ns, success=not busy))
        self._schedule_acquisitions()

    def listen(self, node):
        radio = self.radios[node]
        if radio.state in ("TX", "TX_PREPARE", "CONFIGURE") or not radio.applied:
            return False
        self._advance()
        if radio.rx:
            return True
        radio.token += 1
        radio.cad = None
        radio.state = "RX_SEARCH"
        self._schedule_acquisitions()
        return True

    def _abort_tx(self, node, reason):
        radio = self.radios[node]
        signal = radio.tx
        if signal is None:
            return
        self.active.pop(signal.id, None)
        radio.tx = None
        for receiver, other in self.radios.items():
            other.failed_acquisitions.discard(signal.id)
            if other.rx and other.rx.signal.id == signal.id:
                self._finish_rx(receiver, "tx_aborted")
        self._emit("tx_end", sender=node, signal_id=signal.id, tx_id=signal.id, aborted=True, reason=reason,
                   planned_end_ns=signal.end_ns, airtime_ns=self.engine.now_ns - signal.start_ns)
        if self.tx_ended:
            self.tx_ended(node, signal)

    def sleep(self, node):
        radio = self.radios[node]
        self._advance()
        radio.notification_generation += 1
        radio.config_number += 1
        self._abort_tx(node, "sleep")
        if radio.rx:
            self._finish_rx(node, "sleep", notify=False)
        radio.token += 1
        radio.cad = None
        radio.failed_acquisitions.clear()
        radio.state = "SLEEP"

    def configure(self, node, phy, delay_ns=0, success=True, expected_length=None):
        if type(delay_ns) is not int or delay_ns < 0:
            raise ValueError("configuration delay must be a nonnegative integer")
        radio = self.radios[node]
        radio.notification_generation += 1
        radio.config_number += 1
        generation = radio.config_number
        now = self.engine.now_ns
        at = max(now + delay_ns, radio.tx.end_ns if radio.tx else now)
        if radio.tx is None:
            self._advance()
            if radio.rx:
                self._finish_rx(node, "configure", notify=False)
            radio.token += 1
            radio.cad = None
            radio.state = "CONFIGURE"
        self.engine.schedule(at, COMPLETE, self._configured, node, radio.epoch, generation,
                             phy, success, expected_length)
        return True

    def _configured(self, node, epoch, generation, phy, success, expected_length):
        radio = self.radios[node]
        if radio.epoch != epoch or radio.config_number != generation:
            return
        self._advance()
        radio.token += 1
        radio.applied = bool(success)
        if success:
            radio.phy = phy
            radio.expected_length = expected_length if expected_length is not None else self.config.get("implicit_payload_length")
        radio.state = "RX_SEARCH" if success else "CONFIGURE"
        self._notify(node, Observation("MODE_APPLIED", self.engine.now_ns, success=bool(success)))
        self._schedule_acquisitions()

    def reboot(self, node):
        radio = self.radios[node]
        self._advance()
        radio.epoch += 1
        radio.notification_generation += 1
        radio.token += 1
        radio.config_number += 1
        self._abort_tx(node, "reboot")
        if radio.rx:
            self._finish_rx(node, "reboot", notify=False)
        radio.cad = None
        radio.failed_acquisitions.clear()
        radio.applied = False
        radio.state = "SLEEP"
        self._emit("radio_reboot", node=node)
