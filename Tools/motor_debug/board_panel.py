import time
import tkinter as tk
from tkinter import ttk, messagebox
from board_catalog import CASES, BY_NAME, test_parameters

RESULT_LABELS = {"PASS": "通过", "FAIL_MANUAL": "人工判定失败", "FAIL_COMM": "硬件/通信失败",
                 "FAIL_NO_RESPONSE": "等待输入超时", "CANCELLED": "已取消"}

class BoardPanel:
    def __init__(self, parent, app):
        self.app = app
        self.capable = False
        self.updated = 0.
        self.phase = "IDLE"
        self.active = "-"
        self.sequences = {}
        self.latest = {}
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(1, weight=1)
        self.status = tk.StringVar(value="连接后检测板级测试固件")
        ttk.Label(parent, textvariable=self.status).grid(row=0, column=0, sticky="w", pady=8)
        table = ttk.Frame(parent)
        table.grid(row=1, column=0, sticky="nsew")
        self.tree = ttk.Treeview(table, columns=("item", "label", "result"), show="headings", selectmode="browse")
        for column, text, width in (("item", "项目", 180), ("label", "测试内容", 260), ("result", "本次结果", 180)):
            self.tree.heading(column, text=text)
            self.tree.column(column, width=width)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll = ttk.Scrollbar(table, orient="vertical", command=self.tree.yview)
        scroll.pack(side="right", fill="y")
        self.tree.configure(yscrollcommand=scroll.set)
        for case in CASES:
            self.tree.insert("", "end", iid=case.name, values=(case.name, case.label, "未测试"))
        self.tree.bind("<<TreeviewSelect>>", self.select)
        detail = ttk.LabelFrame(parent, text="所选项目参数", padding=12)
        detail.grid(row=2, column=0, sticky="ew", pady=12)
        self.parameter_title = tk.StringVar(value="参数")
        ttk.Label(detail, textvariable=self.parameter_title).grid(row=0, column=0, padx=5)
        self.parameter = tk.StringVar()
        self.parameter_box = ttk.Spinbox(detail, from_=0, to=20000, textvariable=self.parameter, width=10)
        self.parameter_box.grid(row=0, column=1, padx=8)
        ttk.Label(detail, text="时长 ms").grid(row=0, column=2, padx=8)
        self.duration = tk.StringVar()
        ttk.Spinbox(detail, from_=1, to=60000, textvariable=self.duration, width=10).grid(row=0, column=3)
        self.start_btn = ttk.Button(detail, text="执行所选测试", command=self.start)
        self.start_btn.grid(row=0, column=4, padx=12)
        self.pass_btn = ttk.Button(detail, text="确认通过", command=lambda: self.confirm("PASS"))
        self.pass_btn.grid(row=1, column=0, columnspan=2, pady=10)
        self.fail_btn = ttk.Button(detail, text="标记失败", command=lambda: self.confirm("FAIL"))
        self.fail_btn.grid(row=1, column=2, columnspan=2)
        ttk.Button(detail, text="停止所有输出 / 解锁", command=app.stop).grid(row=1, column=4)
        self.hint = tk.StringVar()
        ttk.Label(detail, textvariable=self.hint, wraplength=820).grid(row=2, column=0, columnspan=5, sticky="w")
        self.key_status = tk.StringVar(value="HOME：等待数据")
        ttk.Label(parent, textvariable=self.key_status).grid(row=3, column=0, sticky="w")
        self.summary = tk.StringVar()
        ttk.Label(parent, textvariable=self.summary).grid(row=4, column=0, sticky="w", pady=6)
        self.tree.selection_set("LOG")
        self.select()
        self.refresh()

    def select(self, _=None):
        selected = self.tree.selection()
        if not selected:
            return
        case = BY_NAME[selected[0]]
        self.parameter_title.set("频率 Hz" if case.kind == "frequency" else "占空比 %" if case.kind == "percent" else "无幅值参数")
        self.parameter.set(str(case.value))
        self.parameter_box["state"] = "disabled" if case.kind == "none" else "normal"
        self.duration.set(str(case.duration))
        self.parameter_box.configure(from_=0, to=100 if case.kind == "percent" else 20000)
        hint = case.hint
        if case.name.startswith("MOTOR_"):
            large = int(case.name[7]) >= 7
            self.parameter_box.configure(from_=10, to=80 if large else 100)
            self.parameter_title.set("大电机 10～80%" if large else "小电机 10～100%")
            hint += " 运行范围：10～80%。" if large else " 运行范围：10～100%。"
        self.hint.set(hint)

    def start(self):
        if not self.app.worker or not self.capable or not self.app.ready:
            return
        selected = self.tree.selection()
        if not selected:
            return
        name = selected[0]
        try:
            value = int(self.parameter.get())
            if BY_NAME[name].kind == "percent":
                value *= 10
            duration = int(self.duration.get())
            test_parameters(name, value, duration)
        except ValueError as exc:
            messagebox.showerror("测试参数", str(exc))
            return
        if not self.app.confirm_exclusive(): return
        self.app.worker.submit("TEST", name, value, duration)

    def confirm(self, result):
        if self.app.worker and self.phase == "WAIT":
            self.app.worker.submit("CONFIRM", result)

    def sent(self, kind, seq, args):
        if kind == "TEST":
            name = args[0]
            self.sequences[seq] = name
            self.latest[name] = seq
            self.tree.set(name, "result", "等待 MCU 接受")

    def receive(self, msg):
        now = time.monotonic()
        if msg[0] == "CAPS":
            self.capable = True
        elif msg[0] == "BOARD":
            _, _, self.active, self.phase = msg
            self.updated = now
        elif msg[0] == "KEY":
            self.key_status.set(f"HOME：{'按下' if msg[2] else '松开'} · 累计按下 {msg[3]} 次")
        elif msg[0] == "RESULT":
            _, seq, name, result = msg
            if self.sequences.get(seq) == name and self.latest.get(name) == seq:
                self.tree.set(name, "result", RESULT_LABELS[result])
                self.sequences.pop(seq, None)
        elif msg[0] == "ACK" and msg[1] in self.sequences:
            name = self.sequences[msg[1]]
            if self.latest.get(name) == msg[1]:
                self.tree.set(name, "result", "已接受 / 测试中")
        elif msg[0] == "ERR" and msg[1] in self.sequences:
            name = self.sequences.pop(msg[1])
            if self.latest.get(name) == msg[1]:
                self.tree.set(name, "result", "拒绝：" + msg[2])

    def disconnect(self):
        self.capable = False
        self.updated = 0.
        self.phase = "IDLE"
        self.active = "-"
        for seq, name in self.sequences.items():
            if self.latest.get(name) == seq:
                self.tree.set(name, "result", "连接中断 / 结果未知")
        self.sequences.clear()
        self.key_status.set("HOME：数据已过期")

    def refresh(self):
        fresh = self.app.ready and self.capable and time.monotonic() - self.updated < .5
        self.start_btn["state"] = "normal" if fresh else "disabled"
        for b in (self.pass_btn, self.fail_btn):
            b["state"] = "normal" if fresh and self.phase == "WAIT" else "disabled"
        if not self.app.ready:
            self.status.set("未连接；结果表保留本次窗口记录")
        elif not self.capable:
            self.status.set("当前固件不支持板级测试，请烧录更新后的 MCU 固件")
        elif not fresh:
            self.status.set("板级测试状态已过期")
            self.key_status.set("HOME：数据已过期")
        else:
            phases = {"IDLE": "空闲", "SETTLE": "等待启动", "DRIVE": "输出中，到时自动停止",
                      "WAIT": "输出已停止，请确认通过或标记失败", "KEY": "等待板上 HOME 按下"}
            self.status.set(f"{self.active if self.active != '-' else '板级测试'} · {phases[self.phase]}")
        values = [self.tree.set(case.name, "result") for case in CASES]
        self.summary.set(f"29 项 · 通过 {values.count('通过')} · 失败 {sum('失败' in s or '超时' in s for s in values)} · 未测试 {values.count('未测试')}")
