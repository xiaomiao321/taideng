"""The desktop side of the line protocol; no GUI or serial dependencies."""
from dataclasses import dataclass


def command(kind, seq, motor=1, duty=800, duration=500):
    if not 0 <= seq <= 65535:
        raise ValueError("Invalid sequence")
    if kind in ("PING", "STOP"):
        return f"{kind} {seq}" + (" ALL\n" if kind == "STOP" else "\n")
    if kind != "RUN" or not 1 <= motor <= 8 or not -1000 <= duty <= 1000 or duty == 0:
        raise ValueError("Invalid motor or duty")
    if not 0 <= duration <= 60000:
        raise ValueError("Invalid duration")
    return f"RUN {seq} M{motor} {duty} {duration}\n"


def parse(line):
    p = line.strip().split()
    if not p:
        return ("LOG", line)
    try:
        if p[0] in ("ACK", "ERR") and len(p) == 3:
            seq = int(p[1])
            if not 0 <= seq <= 65535:
                raise ValueError()
            return p[0], seq, p[2]
        if p[0] == "POT" and len(p) == 8:
            vals = tuple(map(int, p[2:]))
            if any(not 0 <= v <= 4095 for v in vals):
                raise ValueError()
            return "POT", int(p[1]), vals
        if p[0] == "STATE" and len(p) == 6:
            tick, motor, duty, pending, locked = map(int, p[1:])
            if not 0 <= motor <= 8 or not -1000 <= duty <= 1000 or pending not in (0, 1) or locked not in (0, 1):
                raise ValueError()
            return "STATE", tick, motor, duty, bool(pending), bool(locked)
        if p[0] == "ADCERR" and len(p) == 3:
            return "ADCERR", int(p[1]), int(p[2])
        if p[0] == "EVENT" and len(p) == 2:
            return "EVENT", p[1]
    except ValueError:
        return "BAD", line
    return "LOG", line


@dataclass
class PotModel:
    values: tuple | None = None
    baseline: tuple | None = None
    received_at: float = 0.0
    valid: bool = False

    def update(self, values, now):
        self.values, self.received_at, self.valid = values, now, True
        if self.baseline is None:
            self.baseline = values

    def stale(self, now):
        return not self.valid or now - self.received_at > 0.5

    def deltas(self):
        if self.values is None or self.baseline is None:
            return None
        return tuple(a - b for a, b in zip(self.values, self.baseline))
