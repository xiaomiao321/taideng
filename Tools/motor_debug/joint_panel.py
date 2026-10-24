from collections import deque
from pathlib import Path
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from joint_model import Calibration, Nominal, millidegrees, encode_profile, decode_profile
from shell_commands import percent, kilohms

DIRECTIONS = {"正转使角度增大": 1, "正转使角度减小": -1}
MODES = {"开关式": 0, "比例式": 1}
DECAYS = {"滑行": 0, "刹车": 1}
PWM_KHZ = ("1", "2", "4", "5", "10", "20")
SENSOR_DIRECTIONS = {"角度增大时 ADC 增大": 1, "角度增大时 ADC 减小": -1}
FAULTS = {"NONE": "无", "SENSOR": "采样失败/过期", "LIMIT": "超出标定范围", "STALL": "运动无进展",
          "DIRECTION": "方向错误/误差增大", "TIMEOUT": "定位超时", "HARDWARE": "驱动失败"}

class JointPanel:
    def __init__(self, parent, app):
        self.app = app
        self.capable = False
        self.selected = 1
        self.forms, self.device, self.states = {}, {}, {}
        self.limits = {}  # MCU 回报的允许范围（JLIM），键为关节号
        self.motion = None
        self.pending = {}
        self.history = deque(maxlen=150)
        self.last_query = 0.
        self.tunes = {}      # MCU 回报的控制参数（JTUNE），键为关节号
        self.pwm = None      # MCU 回报的 (kHz, 刹车)
        self.tracing = False
        self.trace_at = 0.   # 最近一次收到当前关节 JT 行的时刻
        self.spread = None
        self.notice = tk.StringVar(value="两点标定或理论标定任选其一，标定会保存在 MCU 中；M7/M8 为履带，不做角度控制。")
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
        ttk.Label(box, text="占空比 10～100%").grid(row=2, column=3)
        ttk.Spinbox(box, from_=10, to=100, increment=1, textvariable=self.fields["duty"], width=9).grid(row=2, column=4)
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

        theory = ttk.LabelFrame(parent, text="理论标定 · 电位器 1:1 装在关节轴上，270° 行程；正转方向、占空比、容差沿用上方设置",
                                padding=10)
        theory.pack(fill="x", pady=(8, 0))
        self.nominal = {key: tk.StringVar() for key in
                        ("angle_ref", "raw_ref", "sensor", "ohms", "limit_min", "limit_max")}
        self.nominal["ohms"].set("45")
        ttk.Label(theory, text="参考位置").grid(row=0, column=0, padx=5, pady=4)
        ttk.Label(theory, text="已知角度 °").grid(row=0, column=1)
        ttk.Entry(theory, textvariable=self.nominal["angle_ref"], width=11).grid(row=0, column=2, padx=8)
        ttk.Label(theory, text="ADC").grid(row=0, column=3)
        ttk.Entry(theory, textvariable=self.nominal["raw_ref"], width=9, state="readonly").grid(row=0, column=4, padx=8)
        ttk.Button(theory, text="记录当前位置为参考点", command=self.capture_reference).grid(row=0, column=5, padx=8)
        ttk.Label(theory, text="电位器方向").grid(row=1, column=0, pady=5)
        ttk.Combobox(theory, textvariable=self.nominal["sensor"], values=list(SENSOR_DIRECTIONS), state="readonly",
                     width=20).grid(row=1, column=1, columnspan=2, sticky="w")
        ttk.Label(theory, text="阻值 kΩ").grid(row=1, column=3)
        ttk.Entry(theory, textvariable=self.nominal["ohms"], width=9).grid(row=1, column=4, padx=8)
        ttk.Label(theory, text="留空按 45kΩ").grid(row=1, column=5, sticky="w", padx=8)
        ttk.Label(theory, text="允许范围 °").grid(row=2, column=0, pady=5)
        limits = ttk.Frame(theory)
        limits.grid(row=2, column=1, columnspan=4, sticky="w")
        ttk.Entry(limits, textvariable=self.nominal["limit_min"], width=9).pack(side="left")
        ttk.Label(limits, text="～").pack(side="left", padx=4)
        ttk.Entry(limits, textvariable=self.nominal["limit_max"], width=9).pack(side="left")
        ttk.Label(limits, text="按机构实际能转的范围填写，定位目标只能在此范围内").pack(side="left", padx=8)
        theory_actions = ttk.Frame(theory)
        theory_actions.grid(row=3, column=0, columnspan=6, sticky="w", pady=(8, 0))
        self.nominal_btn = ttk.Button(theory_actions, text="按理论值下发", command=self.apply_nominal)
        self.nominal_btn.pack(side="left")
        self.limit_btn = ttk.Button(theory_actions, text="仅修改允许范围", command=self.apply_limits)
        self.limit_btn.pack(side="left", padx=8)
        self.preview = tk.StringVar(value="")
        ttk.Label(theory_actions, textvariable=self.preview).pack(side="left", padx=12)

        tune = ttk.LabelFrame(parent, text="电机调试 · PWM 上电默认 5kHz 刹车；控制参数随标定存入 MCU，上电自动恢复",
                              padding=10)
        tune.pack(fill="x", pady=(8, 0))
        self.tune_fields = {"khz": tk.StringVar(value="5"), "decay": tk.StringVar(value="刹车"),
                            "mode": tk.StringVar(value="比例式"), "kp": tk.StringVar(value="1"),
                            "low": tk.StringVar(value="30"), "brake": tk.StringVar(value="50")}
        ttk.Label(tune, text="PWM 频率 kHz").grid(row=0, column=0, padx=5, pady=4, sticky="w")
        ttk.Combobox(tune, textvariable=self.tune_fields["khz"], values=PWM_KHZ, state="readonly",
                     width=6).grid(row=0, column=1, sticky="w")
        ttk.Label(tune, text="关断段").grid(row=0, column=2, padx=(12, 5))
        ttk.Combobox(tune, textvariable=self.tune_fields["decay"], values=list(DECAYS), state="readonly",
                     width=6).grid(row=0, column=3, sticky="w")
        self.pwm_btn = ttk.Button(tune, text="应用到全部电机", command=self.apply_pwm)
        self.pwm_btn.grid(row=0, column=4, padx=8)
        self.pwm_status = tk.StringVar(value="MCU：—")
        ttk.Label(tune, textvariable=self.pwm_status).grid(row=0, column=5, columnspan=4, sticky="w")
        ttk.Label(tune, text="控制方式").grid(row=1, column=0, padx=5, pady=4, sticky="w")
        ttk.Combobox(tune, textvariable=self.tune_fields["mode"], values=list(MODES), state="readonly",
                     width=6).grid(row=1, column=1, sticky="w")
        ttk.Label(tune, text="比例 %/°").grid(row=1, column=2, padx=(12, 5))
        ttk.Entry(tune, textvariable=self.tune_fields["kp"], width=6).grid(row=1, column=3, sticky="w")
        ttk.Label(tune, text="起转 %").grid(row=1, column=4, padx=(12, 5))
        ttk.Entry(tune, textvariable=self.tune_fields["low"], width=6).grid(row=1, column=5, sticky="w")
        ttk.Label(tune, text="制动 ms").grid(row=1, column=6, padx=(12, 5))
        ttk.Entry(tune, textvariable=self.tune_fields["brake"], width=6).grid(row=1, column=7, sticky="w")
        self.tune_btn = ttk.Button(tune, text="下发当前关节", command=self.apply_tune)
        self.tune_btn.grid(row=1, column=8, padx=8)
        self.tune_status = tk.StringVar(value="MCU：—")
        ttk.Label(tune, textvariable=self.tune_status).grid(row=2, column=0, columnspan=9, sticky="w")
        record = ttk.Frame(tune)
        record.grid(row=3, column=0, columnspan=9, sticky="w", pady=(6, 0))
        self.trace_btn = ttk.Button(record, text="开始记录", command=self.toggle_trace)
        self.trace_btn.pack(side="left")
        self.trace_status = tk.StringVar(value="每 10ms 记录当前关节的读数、噪声、角度、占空比，存为 records 目录下的 CSV")
        ttk.Label(record, textvariable=self.trace_status).pack(side="left", padx=12)

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

    def read_nominal(self):
        try:
            f = {k: v.get() for k, v in self.fields.items()}
            n = {k: v.get().strip() for k, v in self.nominal.items()}
            return Nominal(int(n["raw_ref"]), millidegrees(n["angle_ref"]), SENSOR_DIRECTIONS[n["sensor"]],
                           DIRECTIONS[f["direction"]], percent(f["duty"]), millidegrees(f["tolerance"]),
                           millidegrees(n["limit_min"]), millidegrees(n["limit_max"]),
                           kilohms(n["ohms"]) if n["ohms"] else 0).validate()
        except (KeyError, ValueError) as exc:
            raise ValueError("请记录参考点，选择电位器方向和正转方向，并填写允许范围。" + str(exc)) from exc

    def read_limits(self):
        try:
            return millidegrees(self.nominal["limit_min"].get()), millidegrees(self.nominal["limit_max"].get())
        except ValueError as exc:
            raise ValueError("请填写允许范围。" + str(exc)) from exc

    def select(self, _=None):
        self.app.stop()
        self.forms[self.selected] = {k: v.get() for k, v in self.fields.items()}
        self.selected = int(self.choice.get()[1])
        self.set_fields(None)
        # 参考点读数属于上一个关节，切换后必须重新记录；范围改用新关节的 MCU 值。
        for key in ("raw_ref", "limit_min", "limit_max"): self.nominal[key].set("")
        self.fill_limits()
        for key, value in self.forms.get(self.selected, {}).items(): self.fields[key].set(value)
        self.history.clear()
        self.query()

    def capture(self, point):
        try:
            millidegrees(self.fields["angle_" + point].get())
            self.fields["raw_" + point].set(str(self.current_raw()))
            self.notice.set(f"已记录 M{self.selected} 的位置 {point.upper()}；角度采用你填写的实测值。")
        except ValueError as exc: messagebox.showerror("记录标定点", str(exc))

    def capture_reference(self):
        try:
            millidegrees(self.nominal["angle_ref"].get())
            self.nominal["raw_ref"].set(str(self.current_raw()))
            self.notice.set(f"已记录 M{self.selected} 的参考点；角度采用你填写的实测值。")
        except ValueError as exc: messagebox.showerror("记录参考点", str(exc))

    def current_raw(self):
        if not self.app.ready or self.app.pot.stale(time.monotonic()):
            raise ValueError("请连接并等待有效 POT 数据")
        if not self.motion or time.monotonic() - self.motion[0] > .5 or self.motion[1][2] or self.motion[1][4]:
            raise ValueError("请先停止电机，待机构稳定后再记录")
        return self.app.pot.values[self.selected - 1]

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
            if not self.app.confirm_exclusive(): return
            self.app.worker.submit("CAL", self.selected, cal.wire())
            self.notice.set("等待 MCU 接受并回读标定；本操作不转动电机。")
        except ValueError as exc: messagebox.showerror("下发标定", str(exc))

    def apply_nominal(self):
        try:
            nominal = self.read_nominal()
            nominal.calibration()
            if not self.capable or not self.app.ready or not self.app.worker:
                raise ValueError("请连接支持关节定位的新固件")
            if not self.app.confirm_exclusive(): return
            self.app.worker.submit("JNOM", self.selected, nominal.wire())
            self.notice.set("等待 MCU 接受并回读理论标定；本操作不转动电机。")
        except ValueError as exc: messagebox.showerror("理论标定", str(exc))

    def apply_limits(self):
        try:
            limit_min, limit_max = self.read_limits()
            cal = self.device.get(self.selected)
            if not cal:
                raise ValueError("MCU 尚未标定当前关节，请先下发标定")
            cal.with_limits(limit_min, limit_max).validate()
            if not self.capable or not self.app.ready or not self.app.worker:
                raise ValueError("请连接支持关节定位的新固件")
            if not self.app.confirm_exclusive(): return
            self.app.worker.submit("JLIM", self.selected, (limit_min, limit_max))
            self.notice.set("等待 MCU 接受新的允许范围；本操作不转动电机。")
        except ValueError as exc: messagebox.showerror("修改允许范围", str(exc))

    def fill_limits(self):
        """表单允许范围为空时，填入 MCU 当前关节的范围。"""
        if self.selected in self.limits and not self.nominal["limit_min"].get() and not self.nominal["limit_max"].get():
            low, high = self.limits[self.selected]
            self.nominal["limit_min"].set(f"{low/1000:g}")
            self.nominal["limit_max"].set(f"{high/1000:g}")

    def device_calibration(self, joint):
        """MCU 当前标定（JCAL）合并允许范围（JLIM）；未收到 JLIM 时按 A～B。"""
        cal = self.device.get(joint)
        if cal and joint in self.limits:
            return cal.with_limits(*self.limits[joint])
        return cal

    def move(self):
        try:
            target = millidegrees(self.target.get())
            timeout = int(self.timeout.get())
            record = self.states.get(self.selected)
            if not self.capable or not self.app.ready or not self.app.worker or not record or time.monotonic() - record[0] > .5:
                raise ValueError("缺少新鲜 MCU 状态")
            cal = self.device_calibration(self.selected)
            if not cal or not record[1][3] or not record[1][4]:
                raise ValueError("MCU 尚未标定当前关节，或传感器读数无效")
            limit_min, limit_max = cal.limits
            if not limit_min <= target <= limit_max:
                raise ValueError(f"目标须位于允许范围 {limit_min/1000:g}～{limit_max/1000:g}° 内")
            if not 500 <= timeout <= 60000:
                raise ValueError("超时范围为 500～60000ms")
            if not self.app.confirm_exclusive(): return
            self.app.worker.submit("JMOVE", self.selected, target, timeout)
            self.notice.set("已请求定位；到位依据真实 POT 反馈与所设容差判定。")
        except ValueError as exc: messagebox.showerror("角度定位", str(exc))

    def apply_pwm(self):
        try:
            if not self.capable or not self.app.ready or not self.app.worker:
                raise ValueError("请连接支持电机调试的新固件")
            if not self.app.confirm_exclusive(): return
            self.app.stop()  # MCU 只在电机全部停止时允许切换
            self.app.worker.submit("MPWM", int(self.tune_fields["khz"].get()), DECAYS[self.tune_fields["decay"].get()])
            self.notice.set("已停止全部输出，等待 MCU 切换 PWM。")
        except (KeyError, ValueError) as exc: messagebox.showerror("电机 PWM", str(exc))

    def read_tune(self):
        f = {k: v.get().strip() for k, v in self.tune_fields.items()}
        mode = MODES[f["mode"]]
        try: brake = int(f["brake"])
        except ValueError: raise ValueError("制动时长须为 0～200ms 的整数") from None
        return mode, percent(f["kp"]), percent(f["low"]), brake

    def apply_tune(self):
        try:
            values = self.read_tune()
            if not self.capable or not self.app.ready or not self.app.worker:
                raise ValueError("请连接支持电机调试的新固件")
            if not self.app.confirm_exclusive(): return
            self.app.worker.submit("JTUNE", self.selected, values)
            self.notice.set("下发控制参数会先停止全部电机，并随标定保存到 MCU；开关式只用标定里的占空比。")
        except (KeyError, ValueError) as exc: messagebox.showerror("控制参数", str(exc))

    def toggle_trace(self):
        if not self.capable or not self.app.ready or not self.app.worker:
            messagebox.showerror("数据记录", "请连接支持电机调试的新固件")
            return
        self.tracing = not self.tracing
        self.app.worker.submit("JTRACE", self.selected, int(self.tracing))

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
        if kind in ("CAL", "JMOVE", "JGET", "JNOM", "JLIM", "JTUNE", "JTRACE", "MPWM"): self.pending[seq] = (kind, args[0])

    def receive(self, msg):
        now = time.monotonic()
        if msg[0] == "JCAPS":
            self.capable = True
            self.query()
            self.app.worker.submit("MPWM", None)
        elif msg[0] == "MPWM":
            self.pwm = (msg[1], msg[2])
        elif msg[0] == "JTUNE":
            self.tunes[msg[2]] = msg[3:]
        elif msg[0] == "JT":
            if msg[2] == self.selected:
                self.trace_at, self.spread = now, msg[4]
                self.history.append((now, msg[5]/1000, msg[6]/1000))
        elif msg[0] == "STATE": self.motion = (now, msg)
        elif msg[0] == "JCAL": self.device[msg[2]] = msg[3]
        elif msg[0] == "JLIM":
            self.limits[msg[2]] = (msg[3], msg[4])
            if msg[2] == self.selected: self.fill_limits()
        elif msg[0] == "JSTATE":
            self.states[msg[2]] = (now, msg)
            if not msg[3]:
                self.device.pop(msg[2], None)
                self.limits.pop(msg[2], None)
            if msg[2] == self.selected and msg[3] and msg[4] and now - self.trace_at > .5:
                self.history.append((now, msg[6]/1000, msg[7]/1000))
        elif msg[0] in ("ACK", "ERR") and msg[1] in self.pending:
            kind, joint = self.pending.pop(msg[1])
            if kind == "JTRACE" and msg[0] == "ERR": self.tracing = False
            label = {"MPWM": "电机 PWM", "JTUNE": f"M{joint} 控制参数", "JTRACE": f"M{joint} 数据记录"}.get(kind, f"M{joint} {kind}")
            self.notice.set(f"{label}：" + ("已接受" if msg[0] == "ACK" else "拒绝：" + msg[2]))
        elif msg[0] == "JRESULT":
            self.notice.set(f"M{msg[2]} 定位：" + {"REACHED": "已在容差内稳定 200ms，停止驱动",
                "CANCELLED": "已取消"}.get(msg[3], "故障停止：" + FAULTS.get(msg[3], msg[3])))

    def disconnect(self):
        self.capable = False; self.device.clear(); self.limits.clear(); self.states.clear(); self.pending.clear()
        self.motion = None; self.tunes.clear(); self.pwm = None; self.tracing = False; self.spread = None

    def refresh(self):
        now = time.monotonic()
        record = self.states.get(self.selected)
        fresh = self.app.ready and self.capable and record and now-record[0] <= .5
        raw = "—" if self.app.pot.stale(now) else str(self.app.pot.values[self.selected - 1])
        angle = f"{record[1][6]/1000:.3f}°" if fresh and record[1][3] and record[1][4] else "—"
        self.live.set(f"M{self.selected} / POT{self.selected}    ADC：{raw}    MCU 实测角度：{angle}")
        device = self.device_calibration(self.selected)
        try: matching = device is not None and self.device[self.selected] == Calibration(*self.read_form().wire())
        except ValueError: matching = False
        if not fresh:
            self.cal_status.set("状态过期 / 未连接")
        elif device is None:
            self.cal_status.set("MCU 未标定当前关节")
        else:
            low, high = device.limits
            self.cal_status.set(f"MCU 已标定 · 允许 {low/1000:g}～{high/1000:g}°" +
                                (" · 与两点表单一致" if matching else ""))
        try:
            n = self.read_nominal(); cal = n.calibration(); low, high = n.endpoints()
            self.preview.set(f"预计：{cal.angle_a/1000:g}°→ADC {cal.raw_a}，{cal.angle_b/1000:g}°→ADC {cal.raw_b}"
                             f"（电位器两端 ADC {low}～{high}）")
        except ValueError as exc:
            self.preview.set("" if not self.nominal["raw_ref"].get() else str(exc).split("。")[-1])
        unlocked = self.motion and now-self.motion[0] <= .5 and not self.motion[1][5]
        ready = self.app.ready and self.capable and unlocked
        for button in (self.apply_btn, self.nominal_btn, self.limit_btn):
            button["state"] = "normal" if ready else "disabled"
        self.pwm_btn["state"] = "normal" if ready else "disabled"
        self.tune_btn["state"] = "normal" if ready else "disabled"
        self.trace_btn["state"] = "normal" if self.app.ready and self.capable else "disabled"
        self.trace_btn["text"] = "停止记录" if self.tracing else "开始记录"
        self.pwm_status.set("MCU：—" if not self.pwm else
                            f"MCU 当前：{self.pwm[0]}kHz {'刹车' if self.pwm[1] else '滑行'}")
        t = self.tunes.get(self.selected)
        self.tune_status.set(f"M{self.selected} MCU 当前：—" if not t else
                             f"M{self.selected} MCU 当前：开关式，占空比用标定值" if t[0] == 0 else
                             f"M{self.selected} MCU 当前：比例式 每度 {t[1]/10:g}% 起转 {t[2]/10:g}% 制动 {t[3]}ms")
        rec = self.app.recorder
        if self.tracing and rec.file:
            noise = "" if self.spread is None else f"  本次采样极差 {self.spread}"
            self.trace_status.set(f"记录中 {Path(rec.path).name}  {rec.rows} 行{noise}")
        elif self.tracing:
            self.trace_status.set("等待 MCU 数据")
        elif rec.path:
            self.trace_status.set(f"上次记录 {rec.path}")
        self.move_btn["state"] = "normal" if fresh and device and record[1][3] and record[1][4] and unlocked else "disabled"
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
