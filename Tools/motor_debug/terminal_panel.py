import time
import tkinter as tk
from tkinter import ttk
from board_catalog import CASES
from shell_commands import HELP, parse_shell, complete

class TerminalPanel:
    def __init__(self, parent, app):
        self.app = app
        self.history = []
        self.cursor = 0
        self.draft = ""
        self.cached = {}
        self.pending_joint = None
        self.watch = tk.BooleanVar(value=False)
        toolbar = ttk.Frame(parent)
        toolbar.pack(fill="x", pady=(0, 8))
        ttk.Label(toolbar, text="MCU 命令终端").pack(side="left")
        ttk.Checkbutton(toolbar, text="显示周期遥测", variable=self.watch).pack(side="left", padx=20)
        ttk.Button(toolbar, text="帮助", command=lambda: self.write(HELP)).pack(side="right")
        ttk.Button(toolbar, text="清屏", command=self.clear).pack(side="right", padx=6)
        ttk.Button(toolbar, text="全部停止", command=app.stop).pack(side="right")
        screen = ttk.Frame(parent)
        screen.pack(fill="both", expand=True)
        self.output = tk.Text(screen, bg="#101820", fg="#d6e2ee", insertbackground="white",
                              font=("Consolas", 11), wrap="word", state="disabled", padx=12, pady=10)
        self.output.pack(side="left", fill="both", expand=True)
        scroll = ttk.Scrollbar(screen, command=self.output.yview)
        scroll.pack(side="right", fill="y")
        self.output.configure(yscrollcommand=scroll.set)
        for tag, color in (("prompt", "#69dbc1"), ("error", "#ff8a89"), ("event", "#ebc96c")):
            self.output.tag_configure(tag, foreground=color)
        row = tk.Frame(parent, bg="#17232e", padx=10, pady=9)
        row.pack(fill="x", pady=(8, 0))
        self.prompt = tk.StringVar(value="mcu@offline:~$")
        tk.Label(row, textvariable=self.prompt, bg="#17232e", fg="#69dbc1",
                 font=("Consolas", 11)).pack(side="left", padx=(0, 10))
        self.entry = tk.Entry(row, bg="#17232e", fg="#f2f6fa", insertbackground="white",
                              relief="flat", font=("Consolas", 12))
        self.entry.pack(side="left", fill="x", expand=True)
        self.entry.bind("<Return>", self.submit)
        self.entry.bind("<Up>", lambda _: self.navigate(-1))
        self.entry.bind("<Down>", lambda _: self.navigate(1))
        self.entry.bind("<Tab>", self.complete)
        self.entry.bind("<Control-l>", lambda _: self.clear())
        self.entry.bind("<Control-c>", self.interrupt)
        self.output.bind("<Control-l>", lambda _: self.clear())
        self.write("全板调试终端 · 输入 help 查看命令；Tab 补全，↑↓ 历史。\n默认隐藏周期遥测；与其他页面共用连接和心跳。", "event")

    def write(self, text, tag=None):
        self.output.configure(state="normal")
        self.output.insert("end", text + "\n", tag or ())
        lines = int(self.output.index("end-1c").split(".")[0])
        if lines > 2000:
            self.output.delete("1.0", f"{lines-1800}.0")
        self.output.see("end")
        self.output.configure(state="disabled")

    def clear(self):
        self.output.configure(state="normal")
        self.output.delete("1.0", "end")
        self.output.configure(state="disabled")
        return "break"

    def navigate(self, delta):
        if self.cursor == len(self.history):
            self.draft = self.entry.get()
        self.cursor = max(0, min(len(self.history), self.cursor + delta))
        self.entry.delete(0, "end")
        self.entry.insert(0, self.history[self.cursor] if self.cursor < len(self.history) else self.draft)
        return "break"

    def complete(self, _=None):
        candidates = complete(self.entry.get())
        if len(candidates) == 1:
            self.entry.delete(0, "end")
            self.entry.insert(0, candidates[0] + " ")
        elif candidates:
            self.write("  ".join(candidates), "event")
        return "break"

    def interrupt(self, _=None):
        self.entry.delete(0, "end")
        self.write("^C → 全部停止", "event")
        self.app.stop()
        return "break"

    def connection(self, connected):
        self.cached.clear()
        self.pending_joint = None
        self.prompt.set(f"mcu@{self.app.port.get() if connected else 'offline'}:~$")
        self.write("已连接，等待新鲜状态数据。" if connected else "连接已断开，缓存数据失效。", "event")

    def receive(self, line, msg):
        kind = msg[0]
        if kind in ("POT", "KEY", "STATE", "BOARD", "JSTATE", "MSTATE", "LSTATE", "LRX"):
            self.cached[kind] = (time.monotonic(), msg, line)
            requested = kind == "JSTATE" and msg[2] == self.pending_joint
            if requested: self.pending_joint = None
            if not self.watch.get() and not requested:
                return
        if kind == "ADCERR":
            self.cached.pop("POT", None)
        self.write(line, "error" if kind in ("ERR", "ADCERR", "BAD") else
                   "event" if kind in ("EVENT", "RESULT") else None)

    def snapshot(self, kind):
        record = self.cached.get(kind)
        if not self.app.ready or not record or time.monotonic() - record[0] > .5:
            raise ValueError("尚无有效数据或数据已过期，请连接并等待 MCU 上报")
        return record[1], record[2]

    def submit(self, _=None):
        text = self.entry.get().strip()
        self.entry.delete(0, "end")
        if not text:
            return "break"
        self.write(self.prompt.get() + " " + text, "prompt")
        if not self.history or self.history[-1] != text:
            self.history.append(text)
            self.history = self.history[-200:]
        self.cursor = len(self.history)
        self.draft = ""
        try:
            cmd = parse_shell(text)
            if cmd.action == "help":
                self.write(HELP)
            elif cmd.action == "clear":
                self.clear()
            elif cmd.action == "history":
                self.write("\n".join(f"{i+1:3}  {s}" for i, s in enumerate(self.history)))
            elif cmd.action == "list":
                self.write("\n".join(f"{c.name:16} {c.label}" for c in CASES))
            elif cmd.action == "watch":
                self.watch.set(cmd.argument == "on")
                self.write("周期遥测：" + ("显示" if self.watch.get() else "隐藏"))
            elif cmd.action in ("adc", "key", "status"):
                msg, line = self.snapshot({"adc": "POT", "key": "KEY", "status": "STATE"}[cmd.action])
                if cmd.action == "adc":
                    self.write("  ".join(f"POT{i+1}={v}" for i, v in enumerate(msg[2])))
                elif cmd.action == "key":
                    self.write(f"HOME={'按下' if msg[2] else '松开'}  累计按下={msg[3]}")
                else:
                    self.write(line)
                    if self.app.board_panel.capable:
                        self.write(self.snapshot("BOARD")[1])
            elif cmd.action == "send":
                if not self.app.ready or not self.app.worker:
                    raise ValueError("尚未连接 MCU，未发送命令")
                if cmd.request[0] in ("TEST", "CONFIRM") and not self.app.board_panel.capable:
                    raise ValueError("当前固件不支持板级测试，请更新 MCU 固件")
                if cmd.request[0] in ("JMOVE", "JGET", "CAL") and not self.app.joint_panel.capable:
                    raise ValueError("当前固件不支持关节定位，请更新 MCU 固件")
                if cmd.request[0] == "JGET": self.pending_joint = cmd.request[1]
                self.app.worker.submit(*cmd.request)
        except ValueError as exc:
            self.write(str(exc), "error")
        return "break"
