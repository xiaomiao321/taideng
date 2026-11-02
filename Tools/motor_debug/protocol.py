"""The desktop side of the line protocol; no GUI or serial dependencies."""
from dataclasses import dataclass
from board_catalog import BY_NAME, test_parameters
from joint_model import Calibration, Nominal, validate_limits
from motor_limits import validate_duty


def command(kind, seq, motor=1, duty=800, duration=500):
    if not 0 <= seq <= 65535:
        raise ValueError("Invalid sequence")
    if kind == "MOFF":
        return f"MOFF {seq}\n"
    if kind == "MSET":
        if type(motor) is not int or not 1 <= motor <= 255 or len(duty) != 8 or len(duration) != 8:
            raise ValueError("多电机参数不完整")
        for i, value in enumerate(duty):
            validate_duty(i+1, value, allow_stop=True)
        if any(type(t) is not int or not 0 <= t <= 60000 for t in duration):
            raise ValueError("运行时间须为 0～60000ms；0 表示连续")
        return f"MSET {seq} {motor} " + " ".join(map(str, (*duty, *duration))) + "\n"
    if kind == "LSET":
        if len(duty) != 7 or any(type(v) is not int or not 0 <= v <= 1000 for v in duty):
            raise ValueError("七路灯光亮度须为 0～100%")
        return f"LSET {seq} " + " ".join(map(str, duty)) + "\n"
    if kind == "MPWM":
        if motor is None:
            return f"MPWM {seq}\n"
        if motor not in (1, 2, 4, 5, 10, 20) or duty not in (0, 1):
            raise ValueError("PWM 频率须为 1、2、4、5、10、20kHz，衰减方式为 coast 或 brake")
        return f"MPWM {seq} {motor} {duty}\n"
    if kind in ("PING", "STOP", "INFO"):
        return f"{kind} {seq}" + (" ALL\n" if kind == "STOP" else "\n")
    if kind in ("CAL", "JMOVE", "JGET", "JNOM", "JLIM", "JTUNE", "JTRACE", "JDIR"):
        if type(motor) is not int or not 1 <= motor <= 6:
            raise ValueError("只有 M1～M6 支持角度控制")
        if kind == "CAL":
            if not isinstance(duty, (tuple, list)) or len(duty) != 7:
                raise ValueError("标定参数不完整")
            values = Calibration(*duty).validate().wire()
            return f"CAL {seq} M{motor} " + " ".join(map(str, values)) + "\n"
        if kind == "JNOM":
            if not isinstance(duty, (tuple, list)) or len(duty) != 9:
                raise ValueError("理论标定参数不完整")
            nominal = Nominal(*duty)
            nominal.calibration()  # 与 MCU 相同的行程检查，越界在发送前拒绝
            return f"JNOM {seq} M{motor} " + " ".join(map(str, nominal.wire())) + "\n"
        if kind == "JLIM":
            if not isinstance(duty, (tuple, list)) or len(duty) != 2:
                raise ValueError("允许范围参数不完整")
            validate_limits(*duty, 0)
            return f"JLIM {seq} M{motor} {duty[0]} {duty[1]}\n"
        if kind == "JGET":
            return f"JGET {seq} M{motor}\n"
        if kind == "JDIR":
            if not isinstance(duty, (tuple, list)) or len(duty) != 3:
                raise ValueError("单向逼近参数不完整")
            direction, backlash, overshoot = duty
            if direction not in (-1, 0, 1):
                raise ValueError("逼近方向须为 +、- 或 0")
            if type(backlash) is not int or not 0 <= backlash <= 30000:
                raise ValueError("背隙须为 0～30°")
            if type(overshoot) is not int or not backlash <= overshoot <= 30000:
                raise ValueError("超调量须不小于背隙，且不超过 30°")
            return f"JDIR {seq} M{motor} {direction} {backlash} {overshoot}\n"
        if kind == "JTRACE":
            if duty not in (0, 1):
                raise ValueError("数据记录参数须为 on 或 off")
            return f"JTRACE {seq} M{motor} {duty}\n"
        if kind == "JTUNE":
            if not isinstance(duty, (tuple, list)) or len(duty) not in (4, 5):
                raise ValueError("控制参数不完整")
            mode, kp, low, brake, *high = duty
            if mode not in (0, 1, 2) or type(kp) is not int or not 0 <= kp <= 1000:
                raise ValueError("比例系数须为 0～100%/度")
            if type(low) is not int or low <= 0:
                raise ValueError("起转占空比须为正数")
            validate_duty(motor, low)
            if type(brake) is not int or not 0 <= brake <= 200:
                raise ValueError("制动时长须为 0～200ms")
            if high:  # 可选的最高占空比，写入该关节标定的占空比
                if type(high[0]) is not int or high[0] <= 0:
                    raise ValueError("最高占空比须为正数")
                validate_duty(motor, high[0])
            return f"JTUNE {seq} M{motor} {mode} {kp} {low} {brake}" + "".join(f" {h}" for h in high) + "\n"
        if type(duty) is not int or not -360000 <= duty <= 360000 or type(duration) is not int or not 500 <= duration <= 60000:
            raise ValueError("目标角度或超时时间不正确（500～60000ms）")
        return f"JMOVE {seq} M{motor} {duty} {duration}\n"
    if kind == "TEST":
        test_parameters(motor, duty, duration)
        return f"TEST {seq} {motor} {duty} {duration}\n"
    if kind == "CONFIRM" and motor in ("PASS", "FAIL"):
        return f"CONFIRM {seq} {motor}\n"
    raise ValueError("Unknown command")


def parse(line):
    p = line.strip().split()
    if not p:
        return ("LOG", line)
    try:
        if len(p) == 3 and p[0] == "CAPS" and p[1] in ("MULTI", "LIGHT") and p[2] == "1":
            return "XCAPS", p[1]
        if p[0] == "JT" and len(p) in (10, 11) and p[2] in tuple(f"M{i}" for i in range(1, 7)):
            tick, raw, spread, angle, target, duty, brake = (int(p[1]), *map(int, p[3:9]))
            if not 0 <= raw <= 4095 or not 0 <= spread <= 4095 or not -1000 <= duty <= 1000 or brake not in (0, 1):
                raise ValueError()
            if p[9] not in ("IDLE", "SETTLE", "MOVE", "DONE", "FAULT"):
                raise ValueError()
            # 参考位置：轨迹式运动中为轨迹参考点，旧固件没有这一项时用目标代替
            ref = int(p[10]) if len(p) == 11 else target
            return "JT", tick, int(p[2][1]), raw, spread, angle, target, duty, bool(brake), p[9], ref
        if p[0] == "JTUNE" and len(p) in (7, 8) and p[2] in tuple(f"M{i}" for i in range(1, 7)):
            mode, kp, low, brake, *high = map(int, p[3:])
            if mode not in (0, 1, 2):
                raise ValueError()
            # 最高占空比：旧固件不报为 None，未标定时固件报 0
            return "JTUNE", int(p[1]), int(p[2][1]), mode, kp, low, brake, (high or [None])[0]
        if p[0] == "JDIR" and len(p) == 6 and p[2] in tuple(f"M{i}" for i in range(1, 7)):
            direction, backlash, overshoot = map(int, p[3:6])
            if direction not in (-1, 0, 1) or not 0 <= backlash <= overshoot <= 30000:
                raise ValueError()
            return "JDIR", int(p[1]), int(p[2][1]), direction, backlash, overshoot
        if p[0] == "MPWM" and len(p) == 3:
            khz, slow = int(p[1]), int(p[2])
            if slow not in (0, 1):
                raise ValueError()
            return "MPWM", khz, bool(slow)
        if p[0] == "LSTATE" and len(p) == 9:
            values = tuple(map(int, p[2:9]))
            if any(not 0 <= v <= 1000 for v in values): raise ValueError()
            return "LSTATE", int(p[1]), values
        if p[0] == "MSTATE" and len(p) == 20:
            values = tuple(map(int, p[2:10])); remaining = tuple(map(int, p[10:18]))
            if any(not -1000 <= v <= 1000 for v in values) or any(not 0 <= t <= 60000 for t in remaining): raise ValueError()
            mask = int(p[18])
            if not 0 <= mask <= 255 or p[19] not in ("0", "1"): raise ValueError()
            return "MSTATE", int(p[1]), values, remaining, mask, p[19] == "1"
        if p == ["CAPS", "JOINT", "1"]:
            return "JCAPS", 1
        if p[0] in ("JSTATE", "JCAL", "JRESULT", "JLIM"):
            if len(p) < 3 or p[2] not in tuple(f"M{i}" for i in range(1, 7)):
                raise ValueError()
            joint = int(p[2][1])
            if p[0] == "JLIM" and len(p) == 5:
                limit_min, limit_max = int(p[3]), int(p[4])
                validate_limits(limit_min, limit_max, 0)
                return "JLIM", int(p[1]), joint, limit_min, limit_max
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
            if p[0] == "JRESULT" and len(p) == 4 and p[3] in ("REACHED", "CANCELLED", "SUPERSEDED", "SENSOR", "LIMIT", "STALL", "DIRECTION", "TIMEOUT", "HARDWARE"):
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
