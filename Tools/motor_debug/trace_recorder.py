"""Write JT trace lines to CSV; no GUI or serial dependencies."""
import csv
import os
import sys
import time

HEADER = ("pc_time_s", "mcu_ms", "joint", "raw", "spread", "angle_deg", "target_deg", "duty_pct", "brake", "phase")
GAP_S = 1.0  # 超过这么久没有 JT 行就结束当前文件，下一行开新文件


def default_folder():
    base = os.path.dirname(sys.executable if getattr(sys, "frozen", False) else os.path.abspath(__file__))
    return os.path.join(base, "records")


class TraceRecorder:
    def __init__(self, folder=None, clock=time.monotonic):
        self.folder = folder or default_folder()
        self.clock = clock
        self.file = self.writer = self.path = None
        self.last = 0.0
        self.rows = 0
        self.joint = None

    def add(self, msg):
        """Record a parsed JT message; return the new file path when one is opened."""
        _, tick, joint, raw, spread, angle, target, duty, brake, phase = msg
        now = self.clock()
        opened = None
        if self.file and (now - self.last > GAP_S or joint != self.joint):
            self.close()
        if not self.file:
            os.makedirs(self.folder, exist_ok=True)
            self.path = os.path.join(self.folder, time.strftime(f"trace_%Y%m%d_%H%M%S_M{joint}.csv"))
            self.file = open(self.path, "w", newline="", encoding="utf-8")
            self.writer = csv.writer(self.file)
            self.writer.writerow(HEADER)
            self.rows = 0
            self.joint = joint
            opened = self.path
        self.last = now
        self.writer.writerow((f"{now:.3f}", tick, f"M{joint}", raw, spread, f"{angle/1000:.3f}", f"{target/1000:.3f}",
                              f"{duty/10:g}", int(brake), phase))
        self.rows += 1
        return opened

    def idle(self):
        """Close the file after a gap; return (path, rows) of the closed file."""
        if self.file and self.clock() - self.last > GAP_S:
            return self.close()
        return None

    def close(self):
        if not self.file:
            return None
        self.file.close()
        done = (self.path, self.rows)
        self.file = self.writer = None
        return done
