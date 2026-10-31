"""Thin ctypes boundary around the same C node and codec used on the boards."""
import ctypes as C
import hashlib
from pathlib import Path
import subprocess

from .types import Phy

ROOT = Path(__file__).resolve().parents[2]
LIBRARY = ROOT / "firmware/build/host/libconvoy_sim.so"


class Config(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in
                ("version", "slot", "slots", "interval_ms", "duty_permille", "listen_one_in")]
    _fields_ += [("nmea_lag_us", C.c_int32), ("nmea_err_us", C.c_int32)]
    _fields_ += [(name, C.c_uint32) for name in
                ("seed", "sf", "bandwidth_hz", "coding_rate", "preamble", "frequency_hz", "sync_word",
                 "experimental_relay", "relay_records")]
    _fields_ += [("relay_ttl_us", C.c_int64), ("implicit_header", C.c_uint32),
                ("crc", C.c_uint32), ("key", C.c_uint8 * 16)]


class NativeRecord(C.Structure):
    _fields_ = [(name, C.c_uint32) for name in ("slot", "session", "counter", "fix_time", "status")]
    _fields_ += [(name, C.c_int32) for name in
                ("lat_e7", "lon_e7", "speed_kmh", "course2", "rssi_dbm", "snr_db4")]
    _fields_ += [("fix_utc_ms", C.c_int64), ("heard_us", C.c_int64)]

    def values(self):
        return {name: getattr(self, name) for name, _ in self._fields_}


class NativeLibrary:
    def __init__(self, build=True):
        if build:
            subprocess.run(["make", "-s", "-C", str(ROOT / "firmware"), "build/host/libconvoy_sim.so"], check=True)
        self.lib = C.CDLL(str(LIBRARY))
        signatures = {
            "sim_abi_version": (C.c_uint32, []),
            "sim_config_size": (C.c_size_t, []), "sim_record_size": (C.c_size_t, []),
            "sim_node_create": (C.c_void_p, [C.POINTER(Config)]),
            "sim_node_destroy": (None, [C.c_void_p]),
            "sim_node_pps": (None, [C.c_void_p, C.c_int64]),
            "sim_node_help": (None, [C.c_void_p, C.c_int]),
            "sim_node_fix": (None, [C.c_void_p, C.c_int64, C.c_int64, C.c_int, C.c_int,
                                     C.c_int32, C.c_int32, C.c_int32, C.c_int32]),
            "sim_node_clock": (C.c_int, [C.c_void_p, C.c_int64, C.POINTER(C.c_int64), C.POINTER(C.c_int64)]),
            "sim_node_next": (C.c_int64, [C.c_void_p, C.c_int64]),
            "sim_node_tx_done": (None, [C.c_void_p, C.c_int64]),
            "sim_node_mode": (None, [C.c_void_p, C.POINTER(Config), C.c_int]),
            "sim_node_view": (C.c_int, [C.c_void_p, C.c_uint, C.POINTER(NativeRecord)]),
            "sim_frame_record": (C.c_int, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_uint, C.POINTER(NativeRecord)]),
            "sim_node_receive": (C.c_uint32, [C.c_void_p, C.c_void_p, C.c_size_t, C.c_int, C.c_int, C.c_int64, C.c_int64]),
            "sim_node_poll": (C.c_size_t, [C.c_void_p, C.c_int64, C.c_void_p, C.c_size_t]),
            "sim_node_duty": (C.c_uint32, [C.c_void_p, C.c_int64]),
            "sim_node_unsure": (C.c_uint32, [C.c_void_p]),
            "sim_node_target_utc": (C.c_int64, [C.c_void_p]),
            "sim_access_enable": (C.c_int, [C.c_void_p, C.c_int64]),
            "sim_access_next": (C.c_int64, [C.c_void_p, C.c_int64]),
            "sim_access_poll": (C.c_uint32, [C.c_void_p, C.c_int64, C.c_void_p, C.c_size_t,
                                            C.POINTER(C.c_size_t)]),
            "sim_access_result": (None, [C.c_void_p, C.c_int64, C.c_int]),
            "sim_access_stat": (C.c_uint64, [C.c_void_p, C.c_uint]),
        }
        for name, (result, args) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, args
        if (self.lib.sim_abi_version() != 1 or self.lib.sim_config_size() != C.sizeof(Config) or
                self.lib.sim_record_size() != C.sizeof(NativeRecord)):
            raise RuntimeError("native simulator ABI does not match the host adapter")
        self.sha256 = hashlib.sha256(LIBRARY.read_bytes()).hexdigest()


class NativeNode:
    def __init__(self, library, config, group_size, group_key, seed, preparation_us=0):
        if config["candidate"] not in ("v3", "relay", "v3-cad", "relay-cad"):
            raise ValueError("unsupported native candidate")
        self.cad_enabled = config["candidate"].endswith("-cad")
        self.lib = library.lib
        self.config = Config()
        self.config.version = 1
        self.config.slot, self.config.slots = config["slot"], group_size
        self.config.interval_ms = config["interval_ms"]
        self.config.duty_permille = config.get("duty_permille", 100)
        self.config.listen_one_in = config.get("listen_one_in", 32)
        self.config.seed = seed
        gnss = config["gnss"]
        self.config.nmea_lag_us = round(gnss["nmea_told_lag_ms"] * 1000)
        self.config.nmea_err_us = round(gnss["nmea_error_ms"] * 1000)
        self.config.experimental_relay = config["candidate"].startswith("relay")
        self.config.relay_records = config.get("relay_records", 2)
        self.config.relay_ttl_us = round(config.get("relay_ttl_s", 30) * 1e6)
        self.config.key[:] = group_key
        self._set_phy(Phy(**config["phy"]))
        self.handle = self.lib.sim_node_create(C.byref(self.config))
        if not self.handle:
            raise ValueError("native node rejected its setup (v3 requires explicit header and CRC)")
        if self.cad_enabled and not self.lib.sim_access_enable(self.handle, preparation_us):
            self.close()
            raise ValueError("invalid declared TX preparation bound for CAD")

    def _set_phy(self, phy):
        for name in ("sf", "bandwidth_hz", "coding_rate", "preamble", "frequency_hz", "sync_word",
                     "implicit_header", "crc"):
            setattr(self.config, name, getattr(phy, name))

    def close(self):
        if self.handle:
            self.lib.sim_node_destroy(self.handle)
            self.handle = None

    def pps(self, local_us):
        self.lib.sim_node_pps(self.handle, local_us)

    def fix(self, local_us, utc_ms, position_valid, time_valid, lat_e7, lon_e7, speed_kmh10, course_deg10):
        bounds = ((local_us, 0, (1 << 63) - 1), (utc_ms, 0, ((1 << 63) - 1) // 1000),
                  (lat_e7, -900000000, 900000000), (lon_e7, -1800000000, 1800000000),
                  (speed_kmh10, -1, (1 << 31) - 1), (course_deg10, -1, 3599))
        if any(type(value) is not int or not low <= value <= high for value, low, high in bounds):
            raise ValueError("sensor value does not fit the actual C GNSS interface")
        self.lib.sim_node_fix(self.handle, local_us, utc_ms, position_valid, time_valid,
                              lat_e7, lon_e7, speed_kmh10, course_deg10)

    def help(self, enabled):
        self.lib.sim_node_help(self.handle, enabled)

    def clock(self, local_us):
        utc, error = C.c_int64(), C.c_int64()
        if not self.lib.sim_node_clock(self.handle, local_us, C.byref(utc), C.byref(error)):
            return None
        return utc.value, error.value

    def next(self, local_us):
        function = self.lib.sim_access_next if self.cad_enabled else self.lib.sim_node_next
        value = function(self.handle, local_us)
        return None if value == (1 << 63) - 1 else value

    def poll(self, local_us):
        output = C.create_string_buffer(255)
        size = self.lib.sim_node_poll(self.handle, local_us, output, len(output))
        return output.raw[:size]

    def access_poll(self, local_us):
        output, length = C.create_string_buffer(255), C.c_size_t()
        flags = self.lib.sim_access_poll(self.handle, local_us, output, len(output), C.byref(length))
        return flags & 255, bool(flags & 256), output.raw[:length.value]

    def cad_result(self, local_us, clear):
        self.lib.sim_access_result(self.handle, local_us, -1 if clear is None else int(clear))

    def receive(self, payload, local_us, rssi_dbm=None, snr_db=None, stamp_us=None):
        # The v3 logic does not select records using signal levels. Preserve unknown in the host
        # observation; use an explicit sentinel only at its mandatory integer C boundary.
        rssi = -32768 if rssi_dbm is None else round(rssi_dbm)
        snr = -32768 if snr_db is None else round(snr_db * 4)
        mask = self.lib.sim_node_receive(self.handle, payload, len(payload), rssi, snr, local_us,
                                         -1 if stamp_us is None else stamp_us)
        return [slot for slot in range(self.config.slots) if mask & (1 << slot)]

    def view(self, slot):
        value = NativeRecord()
        if not self.lib.sim_node_view(self.handle, slot, C.byref(value)):
            return None
        return value.values()

    def records(self, payload):
        result = []
        for index in range(9):
            value = NativeRecord()
            if not self.lib.sim_frame_record(self.handle, payload, len(payload), index, C.byref(value)):
                break
            result.append(value.values())
        return result

    def tx_done(self, local_us):
        self.lib.sim_node_tx_done(self.handle, local_us)

    def target_utc_us(self):
        return self.lib.sim_node_target_utc(self.handle)

    def mode(self, phy, applied):
        if not self.config.experimental_relay and (phy.implicit_header or not phy.crc):
            raise ValueError("native v3 does not implement implicit/no-CRC mode")
        self._set_phy(phy)
        self.lib.sim_node_mode(self.handle, C.byref(self.config), applied)

    def statistics(self, local_us):
        values = {"core_duty_used_ms": self.lib.sim_node_duty(self.handle, local_us),
                  "tx_unsure": self.lib.sim_node_unsure(self.handle)}
        if self.cad_enabled:
            fields = ("access_cad_requests", "access_cad_busy", "access_cad_refused", "access_released",
                      "access_expired", "access_cancelled", "access_exhausted", "access_booked_ms",
                      "access_dropped_booked_ms")
            values.update({name: self.lib.sim_access_stat(self.handle, index) for index, name in enumerate(fields)})
        return values
