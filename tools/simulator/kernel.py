"""A phased discrete-event clock; physical models own same-phase batching."""
import heapq
from dataclasses import dataclass

COMPLETE, WORLD, SENSOR, OBSERVE, DECIDE, START, ACQUIRE = range(7)


@dataclass
class Handle:
    cancelled: bool = False

    def cancel(self):
        self.cancelled = True


class Engine:
    def __init__(self, *, reverse_ties=False, same_time_limit=100000):
        self.now_ns = 0
        self.phase = -1
        self.delta_cycle = 0
        self.reverse_ties = reverse_ties
        self.same_time_limit = same_time_limit
        self._serial = 0
        self._events = []

    def schedule(self, at_ns, phase, callback, *args):
        if type(at_ns) is not int or at_ns < self.now_ns:
            raise ValueError("event time must be integer nanoseconds, not in the past")
        if type(phase) is not int or not COMPLETE <= phase <= ACQUIRE:
            raise ValueError("invalid event phase")
        if not callable(callback):
            raise TypeError("event callback must be callable")
        self._serial += 1
        serial = -self._serial if self.reverse_ties else self._serial
        handle = Handle()
        cycle = self.delta_cycle + int(phase < self.phase) if at_ns == self.now_ns else 0
        heapq.heappush(self._events, (at_ns, cycle, phase, serial, handle, callback, args))
        return handle

    def run(self, until_ns):
        if type(until_ns) is not int or until_ns < self.now_ns:
            raise ValueError("run deadline must be integer nanoseconds, not in the past")
        last_time, count = None, 0
        while self._events and self._events[0][0] <= until_ns:
            at_ns, cycle, phase, _, handle, callback, args = heapq.heappop(self._events)
            if handle.cancelled:
                continue
            count = count + 1 if at_ns == last_time else 1
            last_time = at_ns
            if count > self.same_time_limit:
                raise RuntimeError("too many events without advancing time")
            self.now_ns, self.delta_cycle, self.phase = at_ns, cycle, phase
            callback(*args)
        if self.now_ns < until_ns:
            self.now_ns, self.delta_cycle, self.phase = until_ns, 0, -1
