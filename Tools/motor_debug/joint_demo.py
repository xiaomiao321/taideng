"""Synthetic UI feedback, not a model of a real mechanism."""
import time
from joint_model import Calibration

class DemoJoints:
    def __init__(self, emit):
        self.emit = emit; self.configs = {}; self.focus = 1; self.active = None
        self.targets = {}; self.phases = {}; self.started = 0.; self.seq = 0
        self.stable = None

    def cancel(self):
        if self.active:
            self.emit(f"JRESULT {self.seq} M{self.active} CANCELLED")
            self.phases[self.active] = "IDLE"; self.active = None

    def request(self, kind, seq, joint, value, duration, vals, tick):
        self.focus = joint
        if kind == "CAL":
            self.configs[joint] = Calibration(*value).validate()
            self.targets[joint] = int(self.configs[joint].angle(vals[joint-1]))
            self.phases[joint] = "IDLE"
        if kind == "JMOVE":
            cal = self.configs.get(joint)
            if not cal: return "NOT_CALIBRATED"
            if not cal.angle_a <= value <= cal.angle_b: return "TARGET_RANGE"
            self.targets[joint] = value; self.active = joint; self.seq = seq
            self.started = time.monotonic(); self.timeout = duration/1000; self.stable = None
            self.phases[joint] = "SETTLE"
        if kind in ("CAL", "JGET") and joint in self.configs:
            self.emit(f"JCAL {tick} M{joint} " + " ".join(map(str, self.configs[joint].wire())))
        return None

    def step(self, vals, tick):
        motor = duty = 0
        if self.active:
            joint = self.active; cal = self.configs[joint]
            now = time.monotonic(); angle = cal.angle(vals[joint-1]); error = self.targets[joint]-angle
            if now-self.started > self.timeout:
                self.emit(f"JRESULT {self.seq} M{joint} TIMEOUT")
                self.phases[joint] = "FAULT"; self.active = None
            elif abs(error) <= cal.tolerance:
                if self.stable is None: self.stable = now
                if now-self.stable >= .2:
                    self.emit(f"JRESULT {self.seq} M{joint} REACHED")
                    self.phases[joint] = "DONE"; self.active = None
            elif now-self.started >= .1:
                self.stable = None; self.phases[joint] = "MOVE"
                motor = joint; duty = (1 if error > 0 else -1)*cal.direction*cal.duty
                target_raw = cal.raw_a + (self.targets[joint]-cal.angle_a)*(cal.raw_b-cal.raw_a)/(cal.angle_b-cal.angle_a)
                diff = target_raw-vals[joint-1]
                vals[joint-1] += max(-cal.duty*.03, min(cal.duty*.03, diff))
        joint = self.focus; cal = self.configs.get(joint)
        angle = int(cal.angle(vals[joint-1])) if cal else 0
        phase = self.phases.get(joint, "IDLE")
        self.emit(f"JSTATE {tick} M{joint} {int(cal is not None)} 1 {int(vals[joint-1])} {angle} "
                  f"{self.targets.get(joint,angle)} {duty if motor==joint else 0} {phase} {'TIMEOUT' if phase=='FAULT' else 'NONE'}")
        return motor, duty
