"""Independent manual outputs; device telemetry is kept apart from edited values."""
import json
import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from pathlib import Path
from decimal import Decimal, InvalidOperation
from protocol import command


def pwm(text):
    try:
        value = Decimal(str(text)) * 10
        if not value.is_finite() or value != value.to_integral_value() or not 0 <= value <= 1000:
            raise ValueError()
        return int(value)
    except (InvalidOperation, ValueError):
        raise ValueError("占空比须为 0～100%，最多一位小数") from None


class ChannelPanel:
    def __init__(self, parent, app, motors=False):
        self.app, self.motors = app, motors
        self.capable = False
        self.updated = 0.
        self.actual = None
        self.locked = True
        self.rows, self.buttons = [], []
        self.pending = {}
        self.notice = tk.StringVar(value="连接后检测固件能力；参数修改后点击应用生效。")
        ttk.Label(parent, textvariable=self.notice).pack(anchor="w", pady=5)
        grid = ttk.Frame(parent)
        grid.pack(fill="x")
        headers = ("电机", "勾选 / 启用", "方向", "占空比 %", "调节", "时长 ms，0 连续", "操作", "MCU 输出") if motors else (
            "灯路", "启用", "亮度 %", "调节", "操作", "MCU 输出")
        for c, label in enumerate(headers):
            ttk.Label(grid, text=label).grid(row=0, column=c, padx=7, pady=4, sticky="w")
        names = [f"M{i}" for i in range(1, 9)] if motors else ["UP1", "UP2", "UP3", "UP4", "DOWN1", "DOWN2", "DOWN3"]
        for i, name in enumerate(names):
            row = {"enabled": tk.BooleanVar(value=False), "value": tk.StringVar(value="80" if motors else "50"),
                   "direction": tk.StringVar(value="正转"), "duration": tk.StringVar(value="500"),
                   "status": tk.StringVar(value="—")}
            self.rows.append(row)
            r = i+1
            ttk.Label(grid, text=name).grid(row=r, column=0, padx=7, pady=3, sticky="w")
            ttk.Checkbutton(grid, variable=row["enabled"]).grid(row=r, column=1)
            c = 2
            if motors:
                ttk.Combobox(grid, textvariable=row["direction"], values=("正转", "反转"), state="readonly", width=6).grid(row=r, column=c)
                c += 1
            maximum = 80 if motors and i >= 6 else 100
            ttk.Spinbox(grid, from_=50 if motors else 0, to=maximum, textvariable=row["value"], width=7).grid(row=r, column=c, padx=7)
            scale = ttk.Scale(grid, from_=50 if motors else 0, to=maximum, length=140,
                              command=lambda v, item=row: item["value"].set(str(round(float(v)))))
            scale.set(80 if motors else 50)
            scale.grid(row=r, column=c+1, padx=7)
            c += 2
            if motors:
                ttk.Spinbox(grid, from_=0, to=60000, increment=100, textvariable=row["duration"], width=9).grid(row=r, column=c)
                c += 1
            actions = ttk.Frame(grid)
            actions.grid(row=r, column=c, padx=7)
            for label, action in (("应用", lambda j=i: self.apply(j)), ("停止" if motors else "关闭", lambda j=i: self.off(j))):
                button = ttk.Button(actions, text=label, command=action, width=5)
                button.pack(side="left", padx=2)
                self.buttons.append(button)
            ttk.Label(grid, textvariable=row["status"], width=28).grid(row=r, column=c+1, padx=5, sticky="w")
        foot = ttk.Frame(parent)
        foot.pack(fill="x", pady=8)
        button = ttk.Button(foot, text="应用勾选电机" if motors else "应用全部灯光", command=lambda: self.apply())
        button.pack(side="left")
        self.buttons.append(button)
        ttk.Button(foot, text="停止全部电机" if motors else "全部关灯", command=self.off).pack(side="left", padx=8)
        if not motors:
            self.link = tk.StringVar(value="50")
            ttk.Label(foot, text="勾选路统一亮度 %").pack(side="left", padx=5)
            ttk.Entry(foot, textvariable=self.link, width=6).pack(side="left")
            ttk.Button(foot, text="填入勾选路", command=self.link_values).pack(side="left", padx=5)
            ttk.Button(foot, text="保存预设", command=self.save).pack(side="right")
            ttk.Button(foot, text="载入预设", command=self.load).pack(side="right", padx=5)
        else:
            ttk.Label(parent, text="M1～M6：50～100%；M7/M8：50～80%。每路独立计时，换向先停止 100ms。\n勾选用于成组启动；取消勾选后点击该行应用可停止该路。停止按钮随时可用。未勾选的其他路保持运行。").pack(anchor="w")
        self.refresh()

    def send(self, kind, motor=1, duty=800, duration=500):
        if not self.app.ready or not self.app.worker or not self.capable:
            raise ValueError("请连接支持多路控制的新 MCU 固件")
        command(kind, 0, motor, duty, duration)
        self.app.worker.submit(kind, motor, duty, duration)
        self.notice.set("已排队，等待 MCU 应答和输出回读；未确认前不视为生效。")

    def apply(self, index=None):
        try:
            if time.monotonic() - self.updated > .5 or self.locked:
                raise ValueError("MCU 状态无效或已锁定，请先停止 / 解锁")
            if self.motors:
                if not self.app.confirm_manual(): return
                indices = [index] if index is not None else [i for i, row in enumerate(self.rows) if row["enabled"].get()]
                if not indices: raise ValueError("请先勾选要启动的电机")
                values, times = [0]*8, [0]*8
                for i in indices:
                    row = self.rows[i]
                    values[i] = pwm(row["value"].get()) * (1 if row["direction"].get() == "正转" else -1) if row["enabled"].get() else 0
                    times[i] = int(row["duration"].get())
                self.send("MSET", sum(1 << i for i in indices), tuple(values), tuple(times))
            else:
                values = list(self.actual)
                for i in range(7) if index is None else [index]:
                    row = self.rows[i]
                    values[i] = pwm(row["value"].get()) if row["enabled"].get() else 0
                self.send("LSET", duty=tuple(values))
        except (ValueError, TypeError) as exc: messagebox.showerror("输出参数", str(exc))

    def off(self, index=None):
        try:
            if self.motors:
                if index is None: self.send("MOFF")
                else: self.send("MSET", 1 << index, (0,)*8, (0,)*8)
            else:
                if index is not None and (self.actual is None or time.monotonic()-self.updated > .5):
                    raise ValueError("状态过期，无法保留其他路；可点击全部关灯")
                values = list(self.actual) if index is not None else [0]*7
                if index is not None: values[index] = 0
                self.send("LSET", duty=tuple(values))
        except ValueError as exc: messagebox.showerror("停止输出", str(exc))

    def sent(self, kind, seq, args):
        if kind in (("MSET", "MOFF") if self.motors else ("LSET",)):
            self.pending[seq] = kind

    def receive(self, msg):
        if msg[0] == "XCAPS" and msg[1] == ("MULTI" if self.motors else "LIGHT"):
            self.capable = True
        if msg[0] in ("ACK", "ERR") and msg[1] in self.pending:
            self.pending.pop(msg[1])
            self.notice.set("MCU 已接受，当前输出见右侧回读。" if msg[0] == "ACK" else "MCU 拒绝："+msg[2])
        if msg[0] == ("MSTATE" if self.motors else "LSTATE"):
            self.updated = time.monotonic()
            self.actual = msg[2]
            if self.motors: self.pending_mask = msg[4]
            self.locked = msg[5] if self.motors else False
            for i, (row, value) in enumerate(zip(self.rows, self.actual)):
                if self.motors:
                    text = "换向等待" if msg[4] & (1 << i) else f"{'正转' if value > 0 else '反转'} {abs(value)/10:g}%" if value else "停止"
                    if value and msg[3][i]: text += f" · {msg[3][i]}ms"
                    if self.locked: text = "锁定停止"
                else: text = f"{value/10:g}%" if value else "关闭"
                row["status"].set(text + "（输出回读）")

    def refresh(self):
        fresh = self.app.ready and self.capable and time.monotonic()-self.updated <= .5
        for button in self.buttons: button.configure(state="normal" if fresh and not self.locked else "disabled")
        if not fresh:
            for row in self.rows: row["status"].set("— / 状态无效")

    def disconnect(self):
        self.capable = False; self.updated = 0.; self.actual = None; self.locked = True
        self.pending.clear()
        self.pending_mask = 0
        self.notice.set("未连接；重新连接后不会自动恢复输出。")

    def link_values(self):
        try:
            value = pwm(self.link.get())
            for row in self.rows:
                if row["enabled"].get(): row["value"].set(f"{value/10:g}")
        except ValueError as exc: messagebox.showerror("亮度", str(exc))

    def save(self):
        try:
            values = [pwm(row["value"].get()) for row in self.rows]
            data = {"version": 1, "brightness": values, "enabled": [row["enabled"].get() for row in self.rows]}
            path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("灯光预设", "*.json")])
            if path: Path(path).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        except (OSError, ValueError) as exc: messagebox.showerror("保存预设", str(exc))

    def load(self):
        path = filedialog.askopenfilename(filetypes=[("灯光预设", "*.json")])
        if not path: return
        try:
            data = json.loads(Path(path).read_text(encoding="utf-8"))
            if not isinstance(data, dict) or data.get("version") != 1: raise ValueError("预设版本错误")
            values, enabled = data["brightness"], data["enabled"]
            command("LSET", 0, duty=values)
            if len(enabled) != 7 or any(type(v) is not bool for v in enabled): raise ValueError("预设开关错误")
            for row, value, on in zip(self.rows, values, enabled):
                row["value"].set(f"{value/10:g}"); row["enabled"].set(on)
            self.notice.set("已载入预设；点击应用全部灯光才会输出。")
        except (OSError, ValueError, KeyError, TypeError) as exc: messagebox.showerror("载入预设", str(exc))
