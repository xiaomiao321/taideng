"""Windows motor debug panel. Run: python app.py [--demo]."""
import argparse
from collections import deque
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

from protocol import command, parse, PotModel
from board_panel import BoardPanel
from terminal_panel import TerminalPanel
from joint_panel import JointPanel
from joint_demo import DemoJoints
from channel_panel import ChannelPanel
from linux_panel import LinuxPanel

COLORS = ("#2563eb", "#e45d32", "#159c72", "#995bd6", "#c49c12", "#d74386")


class SerialWorker(threading.Thread):
    def __init__(self, port, events, demo=False):
        super().__init__(daemon=True)
        self.port, self.events, self.demo = port, events, demo
        self.commands = queue.Queue()
        self.quit = threading.Event()
        self.seq = 0

    def submit(self, kind, motor=1, duty=800, duration=500):
        if kind == "STOP":
            while True:
                try:
                    self.commands.get_nowait()
                except queue.Empty:
                    break
        self.commands.put((kind, motor, duty, duration))

    def emit(self, line):
        self.events.put(("line", line))

    def run(self):
        if self.demo:
            self.run_demo()
            return
        serial_port = None
        pending = {}
        try:
            import serial
            serial_port = serial.Serial(self.port, 115200, timeout=0.02, write_timeout=0.1)
            serial_port.reset_input_buffer()
            buf = bytearray()

            def send(kind, motor=1, duty=800, duration=500):
                self.seq = (self.seq + 1) & 65535
                data = command(kind, self.seq, motor, duty, duration).encode("ascii")
                if serial_port.write(data) != len(data):
                    raise OSError("串口发送不完整")
                pending[self.seq] = (time.monotonic(), kind)
                if kind != "PING":
                    self.events.put(("sent", (kind, self.seq, (motor, duty, duration))))
                    self.events.put(("log", "TX " + data.decode().strip()))

            send("PING")
            send("STOP")
            next_ping = time.monotonic() + 0.2
            ready = False
            ping_ok = stop_ok = False
            while not self.quit.is_set():
                now = time.monotonic()
                if any(now - sent > 0.7 for sent, _ in pending.values()):
                    raise TimeoutError("MCU 应答超时，连接已关闭；请检查固件与接线")
                if now >= next_ping:
                    send("PING")
                    next_ping = now + 0.2
                if ready:
                    try:
                        send(*self.commands.get_nowait())
                    except queue.Empty:
                        pass
                buf.extend(serial_port.read(256))
                if len(buf) > 4096:
                    raise ValueError("串口数据行过长")
                while b"\n" in buf:
                    raw, _, buf = buf.partition(b"\n")
                    line = raw.decode("ascii", errors="replace").strip()
                    msg = parse(line)
                    if msg[0] in ("ACK", "ERR"):
                        old = pending.pop(msg[1], None)
                        if old and old[1] == "STOP" and msg[0] == "ACK":
                            stop_ok = True
                        if old and old[1] == "PING" and msg[0] == "ACK":
                            ping_ok = True
                        if ping_ok and stop_ok and not ready:
                            ready = True
                            self.events.put(("connected", self.port))
                            send("INFO")
                        if old and old[1] == "PING" and msg[0] == "ACK":
                            continue
                    self.emit(line)
        except Exception as exc:
            self.events.put(("error", str(exc)))
        finally:
            if serial_port is not None:
                try:
                    serial_port.write(command("STOP", (self.seq + 1) & 65535).encode("ascii"))
                except Exception:
                    pass
                serial_port.close()
            self.events.put(("disconnected", ""))

    def run_demo(self):
        self.events.put(("connected", "模拟模式（未连接硬件）"))
        self.emit("CAPS BOARD_TEST 2")
        self.emit("CAPS JOINT 1")
        self.emit("CAPS MULTI 1")
        self.emit("CAPS LIGHT 1")
        self.emit("CAPS LINUX 1")
        multi = [0]*8
        targets = [0]*8
        due = [0.]*8
        waiting = [0.]*8
        durations = [0]*8
        lamps = [0]*7
        bridge = False
        joints = DemoJoints(self.emit)
        motor, duty, end = 0, 0, 0
        board = None
        vals = [1000., 1500., 2000., 2500., 3000., 3500.]
        start = time.monotonic()
        def finish(result):
            nonlocal board, motor, duty
            self.emit(f"RESULT {board['seq']} {board['name']} {result}")
            board = None
            motor, duty = 0, 0
        while not self.quit.wait(0.1):
            now = time.monotonic()
            try:
                kind, selected, speed, duration = self.commands.get_nowait()
                self.seq = (self.seq + 1) & 65535
                self.events.put(("sent", (kind, self.seq, (selected, speed, duration))))
                if kind in ("STOP", "RUN", "TEST", "CAL", "JMOVE"):
                    joints.cancel()
                    if board:
                        finish("CANCELLED")
                    motor, duty = 0, 0
                    multi[:] = [0]*8; targets[:] = [0]*8; waiting[:] = [0.]*8; lamps[:] = [0]*7
                    bridge = False
                if kind in ("MSET", "MOFF"):
                    joints.cancel()
                    if board: finish("CANCELLED")
                    motor, duty = 0, 0
                    mask = selected if kind == "MSET" else 255
                    for i in range(8):
                        if mask & (1 << i):
                            multi[i] = 0
                            targets[i] = speed[i] if kind == "MSET" else 0
                            durations[i] = duration[i] if kind == "MSET" else 0
                            waiting[i] = now+.1 if targets[i] else 0
                elif kind == "LSET":
                    if board:
                        self.emit(f"ERR {self.seq} BOARD_BUSY")
                        continue
                    lamps[:] = speed
                elif kind == "LMODE": bridge = bool(selected)
                elif kind == "LTX":
                    if not bridge:
                        self.emit(f"ERR {self.seq} BRIDGE_OFF")
                        continue
                    self.emit("LRX "+speed[:32].hex())
                    if len(speed) > 32: self.emit("LRX "+speed[32:].hex())
                if kind in ("CAL", "JMOVE", "JGET"):
                    error = joints.request(kind, self.seq, selected, speed, duration, vals, int((now-start)*1000))
                    if error:
                        self.emit(f"ERR {self.seq} {error}")
                        continue
                if kind == "RUN":
                    motor, duty = selected, speed
                    end = now + duration / 1000 if duration else 0
                elif kind == "TEST":
                    board = dict(seq=self.seq, name=selected, value=speed, duration=duration,
                                 start=now, phase="SETTLE")
                elif kind == "CONFIRM":
                    if board and board['phase'] == 'WAIT':
                        finish("PASS" if selected == "PASS" else "FAIL_MANUAL")
                    else:
                        self.emit(f"ERR {self.seq} NOT_WAITING")
                self.emit(f"ACK {self.seq} {'QUEUED' if kind == 'RUN' else kind}")
            except queue.Empty:
                pass
            if board:
                elapsed = now - board['start']
                name = board['name']
                if name in ("LOG", "ADC6") and elapsed >= .1:
                    if name == "LOG": self.emit("LOGTEST BRINGUP log ok (demo)")
                    finish("PASS")
                elif name == "KEY" and elapsed >= 1:
                    self.emit("KEY 1000 1 1")
                    finish("PASS")
                elif elapsed >= .1 and board['phase'] == 'SETTLE':
                    board['phase'] = 'DRIVE'
                    if name.startswith('MOTOR_'):
                        motor = int(name[7])
                        duty = board['value'] * (1 if name.endswith('FI') else -1)
                        end = 0
                elif elapsed >= .1 + board['duration'] / 1000 * (4 if name == "LED" else 1) and board['phase'] == 'DRIVE':
                    board['phase'] = 'WAIT'
                    motor, duty = 0, 0
                    self.emit(f"TESTWAIT {board['seq']} {name}")
            if end and now >= end and motor and not board:
                motor, duty = 0, 0
                self.emit("EVENT DONE")
            for i in range(8):
                if waiting[i] and now >= waiting[i]:
                    multi[i] = targets[i]; waiting[i] = 0
                    due[i] = now+durations[i]/1000 if durations[i] else 0
                if multi[i] and due[i] and now >= due[i]: multi[i] = targets[i] = 0
                if i < 6 and multi[i]: vals[i] = min(4095, max(0, vals[i]+multi[i]*.04))
            if 1 <= motor <= 6:
                i = motor - 1
                vals[i] = min(4095, max(0, vals[i] + duty * 0.04))
            tick = int((now - start) * 1000)
            joint_motor, joint_duty = joints.step(vals, tick)
            self.emit("POT " + str(tick) + " " + " ".join(str(int(v)) for v in vals))
            active = list(multi)
            if joint_motor or motor: active[(joint_motor or motor)-1] = joint_duty if joint_motor else duty
            focus = next((i+1 for i in range(8) if active[i]), 0)
            pending_mask = sum(1 << i for i in range(8) if waiting[i])
            self.emit(f"STATE {tick} {focus} {active[focus-1] if focus else 0} {int(bool(pending_mask))} 0")
            remaining = [max(0, int((due[i]-now)*1000)) if multi[i] and due[i] else 0 for i in range(8)]
            self.emit(f"MSTATE {tick} "+" ".join(map(str, active+remaining))+f" {pending_mask} 0")
            light_values = list(lamps)
            if board and board['phase'] == 'DRIVE' and board['name'].startswith('LAMP_'):
                light_values[("UP1", "UP2", "UP3", "UP4", "DOWN1", "DOWN2", "DOWN3").index(board['name'][5:])] = board['value']
            self.emit(f"LSTATE {tick} "+" ".join(map(str, light_values))+f" {int(bridge)}")
            self.emit(f"BOARD {tick} {board['name'] if board else '-'} {board['phase'] if board else 'IDLE'}")
            self.emit(f"KEY {tick} 0 0")
        self.events.put(("disconnected", ""))


class App:
    def __init__(self, root, demo=False):
        self.root, self.demo = root, demo
        self.worker = None
        self.events = queue.Queue()
        self.pot = PotModel()
        self.history = deque(maxlen=300)
        self.ready = False
        self.state_received = 0.
        root.title("全板调试上位机" + (" · 模拟模式" if demo else ""))
        root.geometry(f"1180x{min(940, root.winfo_screenheight()-100)}")
        root.minsize(1080, 740)
        root.protocol("WM_DELETE_WINDOW", self.close)
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TButton", padding=7)
        outer = ttk.Frame(root, padding=16)
        outer.pack(fill="both", expand=True)
        connect = ttk.Frame(outer)
        connect.pack(fill="x")
        ttk.Label(connect, text="调试串口 · 115200 / 8N1").pack(side="left")
        self.port = tk.StringVar()
        self.ports = ttk.Combobox(connect, textvariable=self.port, width=17, state="readonly")
        self.ports.pack(side="left", padx=10)
        ttk.Button(connect, text="刷新", command=self.refresh).pack(side="left")
        self.connect_btn = ttk.Button(connect, text="连接", command=self.connect)
        self.connect_btn.pack(side="left", padx=8)
        self.connection = tk.StringVar(value="未连接")
        ttk.Label(connect, textvariable=self.connection).pack(side="left", padx=8)

        notebook = ttk.Notebook(outer)
        self.notebook = notebook
        notebook.pack(fill="both", expand=True, pady=8)
        motor_page = ttk.Frame(notebook)
        board_page = ttk.Frame(notebook, padding=10)
        notebook.add(motor_page, text="电机与电位器")
        notebook.add(board_page, text="板级测试 · 29 项")
        self.board_panel = BoardPanel(board_page, self)
        terminal_page = ttk.Frame(notebook, padding=10)
        notebook.add(terminal_page, text="终端")
        self.terminal = TerminalPanel(terminal_page, self)
        joint_page = ttk.Frame(notebook, padding=10)
        notebook.add(joint_page, text="关节标定与定位")
        self.joint_panel = JointPanel(joint_page, self)
        self.motor_panel = ChannelPanel(motor_page, self, motors=True)
        light_page = ttk.Frame(notebook, padding=10)
        notebook.insert(1, light_page, text="灯光调试 · 7 路 PWM")
        self.light_panel = ChannelPanel(light_page, self)
        linux_page = ttk.Frame(notebook, padding=10)
        notebook.add(linux_page, text="Linux 串口")
        self.linux_panel = LinuxPanel(linux_page, self)
        actions = ttk.Frame(connect)
        actions.pack(side="right")
        ttk.Button(actions, text="停全部电机", command=self.motor_panel.off).pack(side="left", padx=3)
        ttk.Button(actions, text="全部关灯", command=self.light_panel.off).pack(side="left", padx=3)
        tk.Button(actions, text="全部停止 / 解锁", bg="#c62828", fg="white", command=self.stop).pack(side="left", padx=3)
        root.bind("<Escape>", lambda _: self.stop())
        self.motion = tk.StringVar(value="等待 MCU 状态")
        ttk.Label(motor_page, textvariable=self.motion).pack(anchor="w")
        self.run_buttons = []

        pots = ttk.LabelFrame(motor_page, text="电位器 · ADC 原始值 0–4095", padding=10)
        pots.pack(fill="x")
        self.raw_labels, self.delta_labels, self.angle_labels, self.visible = [], [], [], []
        for i, color in enumerate(COLORS):
            pots.columnconfigure(i, weight=1)
            show = tk.BooleanVar(value=True)
            self.visible.append(show)
            ttk.Checkbutton(pots, text=f"POT{i+1}", variable=show,
                            command=self.draw).grid(row=0, column=i)
            raw, delta = tk.StringVar(value="—"), tk.StringVar(value="Δ —")
            self.raw_labels.append(raw)
            self.delta_labels.append(delta)
            tk.Label(pots, textvariable=raw, font=("Segoe UI", 20), fg=color).grid(row=1, column=i)
            ttk.Label(pots, textvariable=delta).grid(row=2, column=i)
            angle = tk.StringVar(value="未标定")
            self.angle_labels.append(angle)
            ttk.Label(pots, textvariable=angle).grid(row=3, column=i)
        foot = ttk.Frame(motor_page)
        foot.pack(fill="x", pady=6)
        self.adc_status = tk.StringVar(value="等待采样")
        ttk.Label(foot, textvariable=self.adc_status).pack(side="left")
        ttk.Button(foot, text="以当前值作为基准", command=self.baseline).pack(side="right")
        self.canvas = tk.Canvas(motor_page, bg="#ffffff", height=130, highlightthickness=1,
                                highlightbackground="#d5dce4")
        self.canvas.pack(fill="both", expand=True, pady=5)
        self.canvas.bind("<Configure>", lambda _: self.draw())
        ttk.Label(motor_page, text="最近 30 秒 · 勾选 POT 显示曲线 · ESC 全部停止 · HOME 后点击全部停止解锁").pack(anchor="w")
        self.log = tk.Text(outer, height=3, font=("Consolas", 10), state="disabled")
        self.log.pack(fill="x", pady=(10, 0))
        def tab_changed(_):
            if notebook.select() == str(terminal_page):
                self.log.pack_forget()
                self.terminal.entry.focus_set()
            else:
                self.log.pack(fill="x", pady=(10, 0))
        notebook.bind("<<NotebookTabChanged>>", tab_changed)
        self.refresh()
        root.after(50, self.poll)

    def refresh(self):
        if self.demo:
            ports = ["DEMO"]
        else:
            try:
                from serial.tools import list_ports
                ports = [p.device for p in list_ports.comports()]
            except ImportError:
                ports = []
                self.connection.set("请先运行启动脚本安装 pyserial")
        self.ports["values"] = ports
        if ports and self.port.get() not in ports:
            self.port.set(ports[0])

    def connect(self):
        if self.worker:
            self.ready = False
            self.worker.quit.set()
            self.connection.set("正在断开…")
            self.connect_btn["state"] = "disabled"
            return
        if not self.port.get():
            messagebox.showinfo("串口", "请选择串口")
            return
        if self.linux_panel.worker and self.port.get().casefold() == self.linux_panel.worker.port.casefold():
            messagebox.showerror("串口占用", "此 COM 口已用于 Linux 调试，请先断开或选择另一串口")
            return
        self.pot = PotModel()
        self.history.clear()
        self.state_received = 0.
        self.ready = False
        self.connection.set("正在连接并确认 MCU…")
        self.connect_btn["state"] = "disabled"
        self.worker = SerialWorker(self.port.get(), self.events, self.demo)
        self.worker.start()

    def confirm_manual(self):
        if self.board_panel.phase != "IDLE":
            return messagebox.askyesno("切换控制", "当前板级测试将取消，停止其输出后切换到手动电机控制，是否继续？")
        joint = self.joint_panel
        if any(record[1][9] in ("SETTLE", "MOVE") for record in joint.states.values()):
            return messagebox.askyesno("切换控制", "当前关节定位将取消，是否切换到手动电机控制？")
        return True

    def confirm_exclusive(self):
        active = any(self.motor_panel.actual or ()) or any(self.light_panel.actual or ())
        active = active or self.board_panel.phase != "IDLE"
        active = active or any(record[1][9] in ("SETTLE", "MOVE") for record in self.joint_panel.states.values())
        active = active or bool(getattr(self.motor_panel, "pending_mask", 0))
        return not active or messagebox.askyesno("切换控制", "此操作会停止当前电机和灯光输出、取消正在进行的测试或定位，是否继续？")

    def stop(self):
        if self.worker:
            self.worker.submit("STOP")

    def baseline(self):
        if not self.pot.stale(time.monotonic()):
            self.pot.baseline = self.pot.values

    def add_log(self, line):
        self.log["state"] = "normal"
        self.log.insert("end", time.strftime("%H:%M:%S ") + line + "\n")
        if int(self.log.index("end-1c").split(".")[0]) > 250:
            self.log.delete("1.0", "50.0")
        self.log.see("end")
        self.log["state"] = "disabled"

    def poll(self):
        for _ in range(200):
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "connected":
                self.ready = True
                self.connection.set("已连接 " + value)
                self.terminal.connection(True)
                self.connect_btn.configure(text="断开", state="normal")
                self.add_log("连接成功，上电/连接后保持停止")
            elif kind == "disconnected":
                self.worker = None
                self.ready = False
                self.pot.valid = False
                self.board_panel.disconnect()
                self.terminal.connection(False)
                self.joint_panel.disconnect()
                self.motor_panel.disconnect()
                self.light_panel.disconnect()
                self.linux_panel.mcu_disconnect()
                self.connection.set("未连接")
                self.motion.set("已断开；MCU 心跳超时后关闭全部测试输出")
                self.connect_btn.configure(text="连接", state="normal")
            elif kind == "sent":
                self.board_panel.sent(*value)
                self.joint_panel.sent(*value)
                self.motor_panel.sent(*value)
                self.light_panel.sent(*value)
                self.linux_panel.sent(*value)
            elif kind in ("error", "log"):
                self.terminal.write(value, "error" if kind == "error" else None)
                self.add_log(value)
            elif kind == "line":
                msg = parse(value)
                self.board_panel.receive(msg)
                self.joint_panel.receive(msg)
                self.motor_panel.receive(msg)
                self.light_panel.receive(msg)
                self.linux_panel.receive(msg)
                self.terminal.receive(value, msg)
                if msg[0] == "POT":
                    self.pot.update(msg[2], time.monotonic())
                    self.history.append((time.monotonic(), msg[2]))
                elif msg[0] == "MSTATE":
                    running = [f"M{i+1} {value/10:+g}%" for i, value in enumerate(msg[2]) if value]
                    self.motion.set("已锁定停止：松开 HOME 后点击全部停止解锁" if msg[5] else
                                    " · ".join(running) + (" · 有电机等待启动" if msg[4] else "") if running or msg[4] else "全部电机停止")
                elif msg[0] == "STATE":
                    self.state_received = time.monotonic()
                    _, _, motor, duty, pending, locked = msg
                    self.motion.set("已锁定停止：松开 HOME 后点击全部停止解锁" if locked else
                                    "换向等待中…" if pending else
                                    f"M{motor} {'正转' if duty > 0 else '反转'} · {abs(duty)/10:g}%（指令占空比）" if motor else
                                    "全部电机停止")
                elif msg[0] == "ADCERR":
                    self.pot.valid = False
                    self.adc_status.set(f"ADC 采样错误 {msg[2]}")
                    self.add_log(value)
                elif msg[0] not in ("MSTATE", "LSTATE", "LRX"):
                    self.add_log(value)
        now = time.monotonic()
        state_fresh = now - self.state_received < 0.5
        for b in self.run_buttons:
            b["state"] = "normal" if self.ready and state_fresh else "disabled"
        if self.ready and not state_fresh:
            self.motion.set("等待有效 MCU 状态 / 状态已过期")
        if self.pot.stale(now):
            self.adc_status.set("采样无效 / 已过期" if self.ready else "未连接 · 数据无效")
            for raw, delta in zip(self.raw_labels, self.delta_labels):
                raw.set("—")
                delta.set("Δ —")
            for label in self.angle_labels: label.set("角度 —")
        else:
            self.adc_status.set("采样正常 · 10Hz")
            for i, delta in enumerate(self.pot.deltas()):
                self.raw_labels[i].set(str(self.pot.values[i]))
                self.delta_labels[i].set(f"Δ {delta:+d}")
                cal = self.joint_panel.device.get(i+1)
                self.angle_labels[i].set(f"{cal.angle(self.pot.values[i])/1000:.1f}°（标定换算）" if cal and 0 < self.pot.values[i] < 4095 else "角度 — / 未标定")
        self.board_panel.refresh()
        self.joint_panel.refresh()
        self.motor_panel.refresh()
        self.light_panel.refresh()
        self.linux_panel.refresh()
        self.draw()
        self.root.after(50, self.poll)

    def draw(self):
        c = self.canvas
        c.delete("all")
        w, h = c.winfo_width(), c.winfo_height()
        left, top, right, bottom = 46, 12, max(60, w - 15), max(50, h - 25)
        for v in (0, 1024, 2048, 3072, 4095):
            y = bottom - v / 4095 * (bottom - top)
            c.create_line(left, y, right, y, fill="#e6ebf1")
            c.create_text(left - 6, y, text=str(v), anchor="e", fill="#65758a")
        now = time.monotonic()
        c.create_text(left, h - 10, text="−30s", anchor="w")
        c.create_text(right, h - 10, text="现在", anchor="e")
        for i, color in enumerate(COLORS):
            if not self.visible[i].get():
                continue
            segment = []
            previous = None
            for t, values in self.history:
                if now - t > 30:
                    continue
                if previous is not None and t - previous > 0.5:
                    if len(segment) >= 4:
                        c.create_line(*segment, fill=color, width=2)
                    segment = []
                segment.extend((right - (now - t) / 30 * (right - left),
                                bottom - values[i] / 4095 * (bottom - top)))
                previous = t
            if len(segment) >= 4:
                c.create_line(*segment, fill=color, width=2)

    def close(self):
        self.linux_panel.close()
        if self.worker:
            self.worker.quit.set()
            self.worker.join(timeout=0.4)
        self.root.destroy()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", action="store_true", help="模拟模式，无需串口或 MCU")
    args = parser.parse_args()
    root = tk.Tk()
    App(root, args.demo)
    root.mainloop()
