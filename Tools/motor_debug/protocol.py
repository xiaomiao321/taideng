"""The desktop side of the line protocol; no GUI or serial dependencies."""
from dataclasses import dataclass
from board_catalog import BY_NAME, test_parameters
from joint_model import Calibration
from motor_limits import validate_duty


def command(kind, seq, motor=1, duty=800, duration=500):
    if not 0 <= seq <= 65535:
        raise ValueError("Invalid sequence")
    if kind in ("PING", "STOP", "INFO"):
        return f"{kind} {seq}" + (" ALL\n" if kind == "STOP" else "\n")
    if kind in ("CAL", "JMOVE", "JGET"):
        if type(motor) is not int or not 1 <= motor <= 6:
            raise ValueError("只有 M1～M6 支持角度控制")
        if kind == "CAL":
            if not isinstance(duty, (tuple, list)) or len(duty) != 7:
                raise ValueError("标定参数不完整")
            values = Calibration(*duty).validate().wire()
            return f"CAL {seq} M{motor} " + " ".join(map(str, values)) + "\n"
        if kind == "JGET":
            return f"JGET {seq} M{motor}\n"
        if type(duty) is not int or not -360000 <= duty <= 360000 or type(duration) is not int or not 500 <= duration <= 60000:
            raise ValueError("目标角度或超时时间不正确（500～60000ms）")
        return f"JMOVE {seq} M{motor} {duty} {duration}\n"
    if kind == "TEST":
        test_parameters(motor, duty, duration)
        return f"TEST {seq} {motor} {duty} {duration}\n"
    if kind == "CONFIRM" and motor in ("PASS", "FAIL"):
        return f"CONFIRM {seq} {motor}\n"
    if kind != "RUN" or not 1 <= motor <= 8 or not -1000 <= duty <= 1000 or duty == 0:
        raise ValueError("Invalid motor or duty")
    validate_duty(motor, duty)
    if not 0 <= duration <= 60000:
        raise ValueError("Invalid duration")
    return f"RUN {seq} M{motor} {duty} {duration}\n"


def parse(line):
    p = line.strip().split()
    if not p:
        return ("LOG", line)
    try:
        if p == ["CAPS", "JOINT", "1"]:
            return "JCAPS", 1
        if p[0] in ("JSTATE", "JCAL", "JRESULT"):
            if len(p) < 3 or p[2] not in tuple(f"M{i}" for i in range(1, 7)):
                raise ValueError()
            joint = int(p[2][1])
            if p[0] == "JCAL" and len(p) == 10:
                cal = Calibration(*map(int, p[3:])).validate()
                return "JCAL", int(p[1]), joint, cal
            if p[0] == "JSTATE" and len(p) == 11:
                calibrated, valid, raw, angle, target, duty = map(int, p[3:9])
                if calibrated not in (0, 1) or valid not in (0, 1) or not 0 <= raw <= 4095 or not -1000 <= duty <= 1000:
                    raise ValueError()
                if p[9] not in ("IDLE", "SETTLE", "MOVE", "DONE", "FAULT") or p[10] not in ("NONE", "SENSOR", "LIMIT", "STALL", "DIRECTION", "TIMEOUT", "HARDWARE"):
                    raise ValueError()
                return "JSTATE", int(p[1]), joint, bool(calibrated), bool(valid), raw, angle, target, duty, p[9], p[10]
            if p[0] == "JRESULT" and len(p) == 4 and p[3] in ("REACHED", "CANCELLED", "SENSOR", "LIMIT", "STALL", "DIRECTION", "TIMEOUT", "HARDWARE"):
                if not 0 <= int(p[1]) <= 65535:
                    raise ValueError()
                return "JRESULT", int(p[1]), joint, p[3]
            raise ValueError()
        if p == ["CAPS", "BOARD_TEST", "2"]:
            return "CAPS", 2
        if p[0] == "BOARD" and len(p) == 4:
            if p[2] not in BY_NAME and p[2] != "-":
                raise ValueError()
            if p[3] not in ("IDLE", "SETTLE", "DRIVE", "WAIT", "KEY"):
                raise ValueError()
            return "BOARD", int(p[1]), p[2], p[3]
        if p[0] == "KEY" and len(p) == 4:
            tick, pressed, count = map(int, p[1:])
            if pressed not in (0, 1) or not 0 <= count <= 0xffffffff:
                raise ValueError()
            return "KEY", tick, bool(pressed), count
        if p[0] == "RESULT" and len(p) == 4:
            seq = int(p[1])
            if not 0 <= seq <= 65535 or p[2] not in BY_NAME or p[3] not in ("PASS", "FAIL_MANUAL", "FAIL_COMM", "FAIL_NO_RESPONSE", "CANCELLED"):
                raise ValueError()
            return "RESULT", seq, p[2], p[3]
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
