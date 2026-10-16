from collections import deque
from pathlib import Path
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from joint_model import Calibration, millidegrees, encode_profile, decode_profile
from shell_commands import percent

DIRECTIONS = {"正转使角度增大": 1, "正转使角度减小": -1}
FAULTS = {"NONE": "无", "SENSOR": "采样失败/过期", "LIMIT": "超出标定范围", "STALL": "运动无进展",
          "DIRECTION": "方向错误/误差增大", "TIMEOUT": "定位超时", "HARDWARE": "驱动失败"}

class JointPanel:
    def __init__(self, parent, app):
        self.app = app
        self.capable = False
        self.selected = 1
        self.forms, self.device, self.states = {}, {}, {}
        self.motion = None
        self.pending = {}
        self.history = deque(maxlen=150)
        self.last_query = 0.
        self.notice = tk.StringVar(value="先记录两个已知角度的位置，再手动下发标定；M7/M8 为履带，不做角度控制。")
        head = ttk.Frame(parent)
        head.pack(fill="x", pady=(0, 8))
        ttk.Label(head, text="关节 / 电位器").pack(side="left")
        self.choice = tk.StringVar(value="M1 / POT1")
        choice = ttk.Combobox(head, textvariable=self.choice, state="readonly", width=14,
                             values=[f"M{i} / POT{i}" for i in range(1, 7)])
        choice.pack(side="left", padx=10)
        choice.bind("<<ComboboxSelected>>", self.select)
        ttk.Button(head, text="保存标定文件", command=self.save).pack(side="right")
        ttk.Button(head, text="载入标定文件", command=self.load).pack(side="right", padx=8)
        self.live = tk.StringVar(value="ADC：—  实测角度：—")
        ttk.Label(parent, textvariable=self.live, font=("Segoe UI", 13)).pack(anchor="w", pady=(0, 8))

        box = ttk.LabelFrame(parent, text="两点标定 · 已知位置 A 的角度须小于位置 B", padding=10)
        box.pack(fill="x")
        self.fields = {key: tk.StringVar() for key in
                       ("raw_a", "angle_a", "raw_b", "angle_b", "direction", "duty", "tolerance")}
        for row, label, key in ((0, "位置 A", "a"), (1, "位置 B", "b")):
            ttk.Label(box, text=label).grid(row=row, column=0, padx=5, pady=4)
            ttk.Label(box, text="已知角度 °").grid(row=row, column=1)
            ttk.Entry(box, textvariable=self.fields["angle_" + key], width=11).grid(row=row, column=2, padx=8)
            ttk.Label(box, text="ADC").grid(row=row, column=3)
            ttk.Entry(box, textvariable=self.fields["raw_" + key], width=9, state="readonly").grid(row=row, column=4, padx=8)
            ttk.Button(box, text="记录当前位置 " + key.upper(), command=lambda k=key: self.capture(k)).grid(row=row, column=5, padx=8)
        ttk.Label(box, text="正转方向").grid(row=2, column=0, pady=5)
        ttk.Combobox(box, textvariable=self.fields["direction"], values=list(DIRECTIONS), state="readonly",
                     width=20).grid(row=2, column=1, columnspan=2, sticky="w")
        ttk.Label(box, text="占空比 50～100%").grid(row=2, column=3)
        ttk.Spinbox(box, from_=50, to=100, increment=1, textvariable=self.fields["duty"], width=9).grid(row=2, column=4)
        tolerance = ttk.Frame(box)
        tolerance.grid(row=2, column=5)
        ttk.Label(tolerance, text="容差 °").pack(side="left")
        ttk.Entry(tolerance, textvariable=self.fields["tolerance"], width=6).pack(side="left", padx=5)
        actions = ttk.Frame(box)
        actions.grid(row=3, column=0, columnspan=6, sticky="w", pady=(8, 0))
        self.apply_btn = ttk.Button(actions, text="下发当前关节标定", command=self.apply)
        self.apply_btn.pack(side="left")
        ttk.Button(actions, text="读取 MCU 参数", command=self.query).pack(side="left", padx=8)
        ttk.Button(actions, text="将 MCU 参数填入表单", command=self.use_device).pack(side="left")
        self.cal_status = tk.StringVar(value="尚未标定")
        ttk.Label(actions, textvariable=self.cal_status).pack(side="left", padx=12)

        move = ttk.LabelFrame(parent, text="角度定位 · MCU 反馈闭环，到位后停止，不自动保持或恢复运动", padding=10)
        move.pack(fill="x", pady=8)
        ttk.Label(move, text="目标角度 °").pack(side="left")
        self.target = tk.StringVar()
        ttk.Entry(move, textvariable=self.target, width=10).pack(side="left", padx=8)
        ttk.Label(move, text="超时 ms").pack(side="left", padx=5)
        self.timeout = tk.StringVar(value="10000")
        ttk.Entry(move, textvariable=self.timeout, width=9).pack(side="left", padx=8)
        self.move_btn = ttk.Button(move, text="开始定位", command=self.move)
        self.move_btn.pack(side="left", padx=8)
        ttk.Button(move, text="停止全部 / 解锁", command=app.stop).pack(side="left")
        ttk.Label(parent, textvariable=self.notice, wraplength=950).pack(anchor="w", pady=4)
        self.state_text = tk.StringVar(value="等待 MCU 关节状态")
        ttk.Label(parent, textvariable=self.state_text).pack(anchor="w")
        self.canvas = tk.Canvas(parent, height=110, bg="white", highlightthickness=1,
                                highlightbackground="#d5dce4")
        self.canvas.pack(fill="both", expand=True, pady=(8, 0))
        self.canvas.bind("<Configure>", lambda _: self.draw())
        self.set_fields(None)
        self.refresh()

    def set_fields(self, cal):
        values = {"raw_a": "", "angle_a": "", "raw_b": "", "angle_b": "", "direction": "", "duty": "80", "tolerance": "2"}
        if cal:
            values = {"raw_a": str(cal.raw_a), "angle_a": f"{cal.angle_a/1000:g}", "raw_b": str(cal.raw_b),
                      "angle_b": f"{cal.angle_b/1000:g}", "direction": next(k for k, v in DIRECTIONS.items() if v == cal.direction),
                      "duty": f"{cal.duty/10:g}", "tolerance": f"{cal.tolerance/1000:g}"}
        for key, value in values.items(): self.fields[key].set(value)

    def read_form(self):
        try:
            f = {k: v.get() for k, v in self.fields.items()}
            return Calibration(int(f["raw_a"]), millidegrees(f["angle_a"]), int(f["raw_b"]),
                               millidegrees(f["angle_b"]), DIRECTIONS[f["direction"]],
                               percent(f["duty"]), millidegrees(f["tolerance"])).validate()
        except (KeyError, ValueError) as exc:
            raise ValueError("请完整记录 A/B 两点并选择正转方向。" + str(exc)) from exc

    def select(self, _=None):
        self.app.stop()
        self.forms[self.selected] = {k: v.get() for k, v in self.fields.items()}
        self.selected = int(self.choice.get()[1])
        self.set_fields(None)
        for key, value in self.forms.get(self.selected, {}).items(): self.fields[key].set(value)
        self.history.clear()
        self.query()

    def capture(self, point):
        try:
            millidegrees(self.fields["angle_" + point].get())
            if not self.app.ready or self.app.pot.stale(time.monotonic()):
                raise ValueError("请连接并等待有效 POT 数据")
            if not self.motion or time.monotonic() - self.motion[0] > .5 or self.motion[1][2] or self.motion[1][4]:
                raise ValueError("请先停止电机，待机构稳定后再记录")
            self.fields["raw_" + point].set(str(self.app.pot.values[self.selected - 1]))
            self.notice.set(f"已记录 M{self.selected} 的位置 {point.upper()}；角度采用你填写的实测值。")
        except ValueError as exc: messagebox.showerror("记录标定点", str(exc))

    def query(self):
        if self.capable and self.app.ready and self.app.worker:
            self.app.worker.submit("JGET", self.selected)
            self.last_query = time.monotonic()

    def use_device(self):
        if self.selected in self.device:
            self.set_fields(self.device[self.selected])
        else:
            self.notice.set("尚无当前关节的 MCU 标定参数，请先读取或下发。")

    def apply(self):
        try:
            cal = self.read_form()
            if not self.capable or not self.app.ready or not self.app.worker:
                raise ValueError("请连接支持关节定位的新固件")
            self.app.worker.submit("CAL", self.selected, cal.wire())
            self.notice.set("等待 MCU 接受并回读标定；本操作不转动电机。")
        except ValueError as exc: messagebox.showerror("下发标定", str(exc))

    def move(self):
        try:
            cal = self.read_form()
            target = millidegrees(self.target.get())
            timeout = int(self.timeout.get())
            record = self.states.get(self.selected)
            if not self.capable or not self.app.ready or not self.app.worker or not record or time.monotonic() - record[0] > .5:
                raise ValueError("缺少新鲜 MCU 状态")
            if self.device.get(self.selected) != cal or not record[1][3] or not record[1][4]:
                raise ValueError("请先下发当前标定，并确认传感器读数有效")
            if not cal.angle_a <= target <= cal.angle_b:
                raise ValueError("目标须位于标定点 A/B 的角度范围内")
            if not 500 <= timeout <= 60000:
                raise ValueError("超时范围为 500～60000ms")
            self.app.worker.submit("JMOVE", self.selected, target, timeout)
            self.notice.set("已请求定位；到位依据真实 POT 反馈与所设容差判定。")
        except ValueError as exc: messagebox.showerror("角度定位", str(exc))

    def save(self):
        try:
            self.forms[self.selected] = {k: v.get() for k, v in self.fields.items()}
            configs = {}
            for joint, f in self.forms.items():
                if not f.get("raw_a") and not f.get("raw_b"): continue
                configs[joint] = Calibration(int(f["raw_a"]), millidegrees(f["angle_a"]), int(f["raw_b"]),
                    millidegrees(f["angle_b"]), DIRECTIONS[f["direction"]], percent(f["duty"]), millidegrees(f["tolerance"])).validate()
            if not configs: raise ValueError("尚无完整标定")
            text = encode_profile(configs)
            path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("标定文件", "*.json")])
            if path: Path(path).write_text(text, encoding="utf-8")
        except (ValueError, KeyError, OSError) as exc: messagebox.showerror("保存标定", str(exc))

    def load(self):
        path = filedialog.askopenfilename(filetypes=[("标定文件", "*.json")])
        if not path: return
        try:
            configs = decode_profile(Path(path).read_text(encoding="utf-8"))
            old = {k: v.get() for k, v in self.fields.items()}
            for joint, cal in configs.items():
                self.set_fields(cal)
                self.forms[joint] = {k: v.get() for k, v in self.fields.items()}
            for k, v in self.forms.get(self.selected, old).items(): self.fields[k].set(v)
            self.notice.set("已载入电脑表单，未下发 MCU、未启动电机。请核对当前样机，再手动下发。")
        except (ValueError, OSError) as exc: messagebox.showerror("载入标定", str(exc))

    def sent(self, kind, seq, args):
        if kind in ("CAL", "JMOVE", "JGET"): self.pending[seq] = (kind, args[0])

    def receive(self, msg):
        now = time.monotonic()
        if msg[0] == "JCAPS":
            self.capable = True
            self.query()
        elif msg[0] == "STATE": self.motion = (now, msg)
        elif msg[0] == "JCAL": self.device[msg[2]] = msg[3]
        elif msg[0] == "JSTATE":
            self.states[msg[2]] = (now, msg)
            if not msg[3]: self.device.pop(msg[2], None)
            if msg[2] == self.selected and msg[3] and msg[4]:
                self.history.append((now, msg[6]/1000, msg[7]/1000))
        elif msg[0] in ("ACK", "ERR") and msg[1] in self.pending:
            kind, joint = self.pending.pop(msg[1])
            self.notice.set(f"M{joint} {kind}：" + ("已接受" if msg[0] == "ACK" else "拒绝：" + msg[2]))
        elif msg[0] == "JRESULT":
            self.notice.set(f"M{msg[2]} 定位：" + {"REACHED": "已在容差内稳定 200ms，停止驱动",
                "CANCELLED": "已取消"}.get(msg[3], "故障停止：" + FAULTS.get(msg[3], msg[3])))

    def disconnect(self):
        self.capable = False; self.device.clear(); self.states.clear(); self.pending.clear(); self.motion = None

    def refresh(self):
        now = time.monotonic()
        record = self.states.get(self.selected)
        fresh = self.app.ready and self.capable and record and now-record[0] <= .5
        raw = "—" if self.app.pot.stale(now) else str(self.app.pot.values[self.selected - 1])
        angle = f"{record[1][6]/1000:.3f}°" if fresh and record[1][3] and record[1][4] else "—"
        self.live.set(f"M{self.selected} / POT{self.selected}    ADC：{raw}    MCU 实测角度：{angle}")
        try: matching = self.device.get(self.selected) == self.read_form()
        except ValueError: matching = False
        self.cal_status.set("MCU 标定与表单一致" if matching and fresh else "未下发 / 表单有变更 / 状态过期")
        unlocked = self.motion and now-self.motion[0] <= .5 and not self.motion[1][5]
        self.apply_btn["state"] = "normal" if self.app.ready and self.capable and unlocked else "disabled"
        self.move_btn["state"] = "normal" if fresh and matching and record[1][3] and record[1][4] and unlocked else "disabled"
        if fresh:
            m = record[1]
            self.state_text.set(f"目标 {m[7]/1000:g}° · 输出 {m[8]/10:g}% · {m[9]} · 故障：{FAULTS[m[10]]}")
        else: self.state_text.set("等待关节状态；旧固件需更新，断线后不会自动恢复定位或下发标定。")
        self.draw()

    def draw(self):
        c = self.canvas; c.delete("all")
        w, h = c.winfo_width(), c.winfo_height()
        points = [(t, a, b) for t, a, b in self.history if time.monotonic()-t <= 15]
        c.create_text(8, 8, text="蓝：实测角度  橙：目标 · 最近 15 秒", anchor="nw", fill="#526479")
        if not points or w < 100 or h < 50: return
        low = min(min(a, b) for _, a, b in points)-1
        high = max(max(a, b) for _, a, b in points)+1
        now = time.monotonic()
        for index, color in ((1, "#2563eb"), (2, "#e45d32")):
            segment = []; previous = None
            for p in points:
                if previous is not None and p[0]-previous > .5:
                    if len(segment) >= 4: c.create_line(*segment, fill=color, width=2)
                    segment = []
                segment.extend((40+(w-50)*(1-(now-p[0])/15), 28+(h-40)*(high-p[index])/(high-low)))
                previous = p[0]
            if len(segment) >= 4: c.create_line(*segment, fill=color, width=2)
