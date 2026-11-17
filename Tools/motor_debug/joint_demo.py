"""Synthetic UI feedback, not a model of a real mechanism."""
import time
from joint_model import Calibration, Nominal

class DemoJoints:
    def __init__(self, emit):
        self.emit = emit; self.configs = {}; self.focus = 1
        self.targets = {}; self.phases = {}; self.round = 0
        self.active = {}  # 关节号 -> [seq, 开始时刻, 超时秒, 进入容差时刻]
        # 与固件默认值一致：比例积分式；JTUNE 为方式、比例、起转、制动，JPI 为 15 项，JDIR 为方向、背隙、超调。
        self.tunes = {j: (3, 10, 300, 50) for j in range(1, 7)}
        self.pis = {j: (400, 150, 80, 200, 300, 700)*2 + (8000, 80, 0) for j in range(1, 7)}
        self.dirs = {j: (0, 0, 0) for j in range(1, 7)}
        self.holds = {}   # 关节号 -> 判到达的时刻；比例积分式到位后保持 1 秒

    def cancel(self):
        for joint, (seq, *_) in self.active.items():
            self.emit(f"JRESULT {seq} M{joint} CANCELLED")
            self.phases[joint] = "IDLE"
        self.active.clear()
        for joint in self.holds: self.phases[joint] = "IDLE"
        self.holds.clear()

    def request(self, kind, seq, joint, value, duration, vals, tick):
        if kind != "JMOVE": self.focus = joint
        if kind in ("CAL", "JNOM"):
            try:
                cal = (Calibration(*value) if kind == "CAL" else Nominal(*value).calibration()).validate()
            except ValueError:
                return "CAL_PARAM"
            self.configs[joint] = cal
            self.targets[joint] = int(cal.angle(vals[joint-1]))
            self.phases[joint] = "IDLE"
        if kind == "JLIM":
            cal = self.configs.get(joint)
            if not cal: return "NOT_CALIBRATED"
            try: self.configs[joint] = cal.with_limits(*value).validate()
            except ValueError: return "LIMIT_PARAM"
        if kind == "JMOVE":
            cal = self.configs.get(joint)
            if not cal: return "NOT_CALIBRATED"
            if not cal.limits[0] <= value <= cal.limits[1]: return "TARGET_RANGE"
            if joint in self.active: self.emit(f"JRESULT {self.active[joint][0]} M{joint} SUPERSEDED")
            self.targets[joint] = value; self.active[joint] = [seq, time.monotonic(), duration/1000, None]
            self.phases[joint] = "SETTLE"; self.holds.pop(joint, None)
        if kind == "JTUNE": self.tunes[joint] = tuple(value[:4])
        if kind == "JPI": self.pis[joint] = tuple(value)
        if kind == "JDIR": self.dirs[joint] = tuple(value)
        if kind in ("CAL", "JGET", "JNOM", "JLIM") and joint in self.configs:
            cal = self.configs[joint]
            self.emit(f"JCAL {tick} M{joint} " + " ".join(map(str, cal.wire())))
            self.emit(f"JLIM {tick} M{joint} {cal.limits[0]} {cal.limits[1]}")
        if kind in ("JGET", "JTUNE", "JPI", "JDIR"):
            high = self.configs[joint].duty if joint in self.configs else 0
            self.emit(f"JTUNE {tick} M{joint} " + " ".join(map(str, (*self.tunes[joint], high))))
            self.emit(f"JDIR {tick} M{joint} " + " ".join(map(str, self.dirs[joint])))
            self.emit(f"JPI {tick} M{joint} " + " ".join(map(str, self.pis[joint])))
        return None

    def step(self, vals, tick):
        motor = duty = 0
        duties = {}
        now = time.monotonic()
        for joint, motion in list(self.active.items()):
            seq, started, timeout, stable = motion
            cal = self.configs[joint]; angle = cal.angle(vals[joint-1]); error = self.targets[joint]-angle
            if now-started > timeout:
                self.emit(f"JRESULT {seq} M{joint} TIMEOUT")
                self.phases[joint] = "FAULT"; del self.active[joint]
            elif abs(error) <= cal.tolerance:
                if stable is None: motion[3] = now
                elif now-stable >= .2:
                    self.emit(f"JRESULT {seq} M{joint} REACHED")
                    del self.active[joint]
                    if self.tunes[joint][0] == 3: self.phases[joint] = "HOLD"; self.holds[joint] = now
                    else: self.phases[joint] = "DONE"
            elif now-started >= .1:
                motion[3] = None; self.phases[joint] = "MOVE"
                duties[joint] = (1 if error > 0 else -1)*cal.direction*cal.duty
                target_raw = cal.raw_a + (self.targets[joint]-cal.angle_a)*(cal.raw_b-cal.raw_a)/(cal.angle_b-cal.angle_a)
                diff = target_raw-vals[joint-1]
                vals[joint-1] += max(-cal.duty*.03, min(cal.duty*.03, diff))
                if not motor: motor, duty = joint, duties[joint]
        for joint, since in list(self.holds.items()):
            if now-since >= 1: self.phases[joint] = "DONE"; del self.holds[joint]
        # 与固件一致：焦点关节和运动中的关节每次都报，其余轮流补报一个。
        self.round = self.round % 6 + 1
        for joint in range(1, 7):
            if joint not in (self.focus, self.round) and joint not in self.active and joint not in self.holds: continue
            cal = self.configs.get(joint)
            angle = int(cal.angle(vals[joint-1])) if cal else 0
            phase = self.phases.get(joint, "IDLE")
            self.emit(f"JSTATE {tick} M{joint} {int(cal is not None)} 1 {int(vals[joint-1])} {angle} "
                      f"{self.targets.get(joint,angle)} {duties.get(joint, 0)} {phase} {'TIMEOUT' if phase=='FAULT' else 'NONE'}")
        return motor, duty
