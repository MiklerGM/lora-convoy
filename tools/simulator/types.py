"""Values crossing model boundaries; evaluator provenance never enters observations."""
from dataclasses import dataclass

NS = 1_000_000_000
US = 1_000


@dataclass(frozen=True)
class Phy:
    frequency_hz: int = 869525000
    bandwidth_hz: int = 250000
    sf: int = 7
    coding_rate: int = 5
    preamble: int = 8
    implicit_header: bool = False
    crc: bool = True
    sync_word: int = 0x6474

    def __post_init__(self):
        bounds = {"frequency_hz": (150000000, 960000000), "sf": (7, 12),
                  "coding_rate": (5, 8), "preamble": (6, 65535), "sync_word": (0, 65535)}
        for name, (low, high) in bounds.items():
            value = getattr(self, name)
            if type(value) is not int or not low <= value <= high:
                raise ValueError(f"invalid PHY {name}")
        if type(self.bandwidth_hz) is not int or self.bandwidth_hz not in (62500, 125000, 250000, 500000):
            raise ValueError("unsupported PHY bandwidth_hz")
        if type(self.implicit_header) is not bool or type(self.crc) is not bool:
            raise ValueError("PHY header and CRC flags must be boolean")

    def airtime_ns(self, length):
        if type(length) is not int or not 0 <= length <= 255:
            raise ValueError("radio payload must contain 0..255 bytes")
        symbol_ns = (1 << self.sf) * NS // self.bandwidth_hz
        de = int(symbol_ns >= 16_380_000)
        bits = 8 * length - 4 * self.sf + 28 + 16 * self.crc - 20 * self.implicit_header
        divisor = 4 * (self.sf - 2 * de)
        blocks = max(0, (bits + divisor - 1) // divisor)
        return (4 * self.preamble + 17 + 4 * (8 + blocks * self.coding_rate)) * symbol_ns // 4


@dataclass(frozen=True)
class Link:
    decode: bool = True
    sense: bool = True
    interfere: bool = True
    power_dbm: float | None = None


@dataclass(frozen=True)
class Observation:
    kind: str
    at_ns: int
    payload: bytes = b""
    rssi_dbm: float | None = None
    snr_db: float | None = None
    success: bool = True


@dataclass(frozen=True)
class Record:
    key: str
    author: str
    boot: int
    created_ns: int
    fix_id: str | None
    state_event: str | None
    position: tuple[float, float, float]
    state: str
