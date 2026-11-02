"""A second, independent serial session to Linux or to the MCU USART1."""
import codecs
from collections import deque
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog
from pathlib import Path
from linux_protocol import encode, Decoder, describe


class RawSerial(threading.Thread):
    def __init__(self, port, settings, events):
        super().__init__(daemon=True)
        self.port, self.settings, self.events = port, settings, events
        self.quit = threading.Event()
        self.commands = queue.Queue(maxsize=64)

    def emit(self, event):
        self.events.put(event, timeout=.05)

    def force_event(self, event):
        while True:
            try:
                self.events.put_nowait(event)
                return
            except queue.Full:
                try: self.events.get_nowait()
                except queue.Empty: pass

    def run(self):
        try:
            import serial
            with serial.Serial(self.port, timeout=.02, write_timeout=.2, **self.settings) as session:
                self.emit(("open", self.port))
                while not self.quit.is_set():
                    try: data = self.commands.get_nowait()
                    except queue.Empty: data = None
                    if data is not None:
                        if session.write(data) != len(data): raise OSError("发送不完整，未重试")
                        self.emit(("tx", data))
                    data = session.read(512)
                    if data: self.emit(("rx", data))
        except Exception as exc:
            # Preserve a visible error even when the UI cannot keep up.
            self.force_event(("error", "接收队列溢出，会话关闭" if isinstance(exc, queue.Full) else str(exc)))
        finally:
            self.force_event(("closed", ""))


class LinuxPanel:
    def __init__(self, parent, app):
        self.app = app
        self.worker = None
        self.events = queue.Queue(maxsize=2048)
        self.frames = {}
        self.seq = 0
        self.decoder = Decoder()
        self.text_decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self.history = deque(maxlen=200)
        self.tx_count = self.rx_count = 0
        self.logs = deque(maxlen=10000)
        self.port = tk.StringVar()
        self.baud = tk.StringVar(value="115200")
        self.bits = tk.StringVar(value="8")
        self.parity = tk.StringVar(value="N")
        self.stops = tk.StringVar(value="1")
        top = ttk.Frame(parent); top.pack(fill="x", pady=5)
        self.ports = ttk.Combobox(top, textvariable=self.port, width=12, state="readonly"); self.ports.pack(side="left", padx=5)
        ttk.Button(top, text="刷新", command=self.refresh_ports).pack(side="left")
        for variable, choices, width in ((self.baud, ("9600", "19200", "38400", "57600", "115200", "230400"), 8),
                                          (self.bits, ("7", "8"), 2), (self.parity, ("N", "E", "O"), 2), (self.stops, ("1", "2"), 2)):
            ttk.Combobox(top, textvariable=variable, values=choices, width=width, state="readonly").pack(side="left", padx=3)
        ttk.Button(top, text="连接", command=self.connect).pack(side="left", padx=5)
        ttk.Button(top, text="断开", command=self.disconnect).pack(side="left")
        self.status = tk.StringVar(value="未连接")
        ttk.Label(parent, textvariable=self.status).pack(anchor="w", pady=5)
        ttk.Label(parent, text="另用一个 USB 转串口，连接 Linux 串口控制台，或直连 MCU USART1 测试 Android 协议。\n协议主机模式需要对端提供 MCU 协议响应；Linux 控制台通常使用普通文本。同一条链路只保留一个控制主机。").pack(anchor="w")
        self.output = tk.Text(parent, height=14, bg="#101923", fg="#d9e5ef", font=("Consolas", 10), state="disabled")
        self.output.pack(fill="both", expand=True, pady=8)
        options = ttk.Frame(parent); options.pack(fill="x")
        self.display = tk.StringVar(value="HEX + 文本")
        ttk.Combobox(options, textvariable=self.display, values=("HEX + 文本", "HEX", "文本"), state="readonly", width=14).pack(side="left")
        ttk.Button(options, text="清屏", command=self.clear).pack(side="left", padx=5)
        ttk.Button(options, text="保存日志", command=self.save).pack(side="left")
        self.counts = tk.StringVar(value="TX 0 B / RX 0 B")
        ttk.Label(options, textvariable=self.counts).pack(side="right")
        raw = ttk.LabelFrame(parent, text="普通消息发送", padding=8); raw.pack(fill="x", pady=7)
        self.input_mode = tk.StringVar(value="文本 UTF-8")
        self.newline = tk.StringVar(value="LF")
        ttk.Combobox(raw, textvariable=self.input_mode, values=("文本 UTF-8", "HEX"), state="readonly", width=12).pack(side="left")
        self.entry = ttk.Combobox(raw); self.entry.pack(side="left", fill="x", expand=True, padx=5)
        self.entry.bind("<Return>", lambda _: self.send_raw())
        ttk.Combobox(raw, textvariable=self.newline, values=("无", "LF", "CRLF", "CR"), state="readonly", width=6).pack(side="left")
        ttk.Button(raw, text="发送", command=self.send_raw).pack(side="left", padx=5)
        protocol = ttk.LabelFrame(parent, text="SDK 现有协议 v2 · 自动序号 / CRC16-MODBUS · 不自动重发", padding=8)
        protocol.pack(fill="x")
        self.op = tk.StringVar(value="PING")
        ttk.Combobox(protocol, textvariable=self.op, values=("PING", "GET_STATUS", "SET_LIGHT", "STOP", "FORWARD", "BACKWARD", "TURN_LEFT", "TURN_RIGHT", "自定义"), state="readonly", width=15).grid(row=0, column=0)
        self.amount = tk.StringVar(value="80")
        ttk.Label(protocol, text="亮度 / 速度 %").grid(row=0, column=1, padx=5)
        ttk.Entry(protocol, textvariable=self.amount, width=5).grid(row=0, column=2)
        self.light_on = tk.BooleanVar(value=True)
        ttk.Checkbutton(protocol, text="灯光开", variable=self.light_on).grid(row=0, column=3, padx=5)
        self.custom_cmd = tk.StringVar(value="31")
        self.payload = tk.StringVar()
        ttk.Label(protocol, text="自定义 CMD(hex)").grid(row=1, column=0, pady=6)
        ttk.Entry(protocol, textvariable=self.custom_cmd, width=6).grid(row=1, column=1)
        ttk.Label(protocol, text="负载 HEX").grid(row=1, column=2, padx=5)
        ttk.Entry(protocol, textvariable=self.payload, width=40).grid(row=1, column=3, columnspan=3, sticky="ew")
        ttk.Button(protocol, text="预览帧", command=self.preview).grid(row=0, column=4, padx=5)
        ttk.Button(protocol, text="发送协议帧", command=self.send_protocol).grid(row=0, column=5)
        self.preview_text = tk.StringVar(value="自定义项按原协议填负载；扩展命令是否支持以对端 ERR/状态回读为准。")
        ttk.Label(protocol, textvariable=self.preview_text, wraplength=1000).grid(row=2, column=0, columnspan=6, sticky="w")
        self.refresh_ports()
        self.refresh()

    def refresh_ports(self):
        try:
            from serial.tools import list_ports
            ports = [p.device for p in list_ports.comports()]
        except ImportError: ports = []
        self.ports.configure(values=ports)
        if ports and self.port.get() not in ports: self.port.set(ports[0])

    def reset_stream(self):
        self.decoder = Decoder(); self.text_decoder.reset(); self.frames.clear()

    def connect(self):
        try:
            if self.worker: raise ValueError("请先断开当前会话")
            self.reset_stream()
            port = self.port.get()
            if not port: raise ValueError("请选择 Linux 串口")
            if self.app.worker and port.casefold() == self.app.worker.port.casefold(): raise ValueError("此 COM 口已用于 MCU 调试，请选另一串口")
            settings = dict(baudrate=int(self.baud.get()), bytesize=int(self.bits.get()), parity=self.parity.get(), stopbits=int(self.stops.get()))
            self.worker = RawSerial(port, settings, self.events)
            self.worker.start()
            self.status.set("正在打开独立串口…")
        except ValueError as exc: messagebox.showerror("Linux 串口", str(exc))

    def disconnect(self):
        if self.worker: self.worker.quit.set()
        self.reset_stream()
        self.status.set("已断开；等待中的协议请求已清除。")

    def write(self, text):
        line = time.strftime("%H:%M:%S ") + text
        self.logs.append(line)
        self.output.configure(state="normal"); self.output.insert("end", line+"\n")
        count = int(self.output.index("end-1c").split(".")[0])
        if count > 2000: self.output.delete("1.0", f"{count-1800}.0")
        self.output.see("end"); self.output.configure(state="disabled")

    def traffic(self, direction, data):
        if direction == "RX": self.rx_count += len(data)
        else: self.tx_count += len(data)
        mode = self.display.get()
        text = self.text_decoder.decode(data) if direction == "RX" else data.decode("utf-8", errors="replace")
        if "HEX" in mode: self.write(direction+" HEX "+data.hex(" ").upper())
        if mode != "HEX": self.write(direction+" TXT "+repr(text))
        self.counts.set(f"TX {self.tx_count} B / RX {self.rx_count} B")
        if direction == "RX":
            old_errors = self.decoder.errors
            for frame in self.decoder.feed(data):
                self.write("协议 "+describe(frame))
                seq, cmd, flags, payload = frame
                pending = self.frames.get(seq)
                if pending and flags & 1 and ((cmd == 0x7f and len(payload) == 4 and payload[0] == pending[1]) or (cmd == 0x21 and pending[1] == 0x20)):
                    self.frames.pop(seq)
            if self.decoder.errors != old_errors: self.write("协议校验错误，已丢弃并重新寻找帧头。")

    def send_bytes(self, data):
        if not data: raise ValueError("消息不能为空")
        if not self.worker or not self.worker.is_alive(): raise ValueError("请先连接独立串口")
        if len(data) > 4096: raise ValueError("每次最多发送 4096 字节")
        try: self.worker.commands.put_nowait(data)
        except queue.Full: raise ValueError("发送队列已满") from None

    def send_raw(self):
        try:
            text = self.entry.get()
            if self.input_mode.get() == "HEX": data = bytes.fromhex(text)
            else: data = text.encode("utf-8") + {"无": b"", "LF": b"\n", "CRLF": b"\r\n", "CR": b"\r"}[self.newline.get()]
            self.send_bytes(data)
            if text not in self.history: self.history.append(text)
            self.entry.configure(values=tuple(reversed(self.history)))
        except ValueError as exc: messagebox.showerror("发送消息", str(exc))

    def build_frame(self):
        op = self.op.get()
        if op == "PING": cmd, payload, flags = 1, int(time.monotonic()*1000 % 2**32).to_bytes(4, "little"), 2
        elif op == "GET_STATUS": cmd, payload, flags = 0x20, b"", 0
        elif op == "自定义": cmd, payload, flags = int(self.custom_cmd.get(), 16), bytes.fromhex(self.payload.get()), 2
        elif op == "STOP": cmd, payload, flags = 0x11, bytes(4), 2
        else:
            amount = int(self.amount.get())
            if not 0 <= amount <= 100: raise ValueError("亮度 / 速度须为 0～100")
            if op == "SET_LIGHT": cmd, payload = 0x10, bytes((int(self.light_on.get()), amount, 0, 0))
            else: cmd, payload = 0x11, bytes((("STOP", "FORWARD", "BACKWARD", "TURN_LEFT", "TURN_RIGHT").index(op), amount, 0, 0))
            flags = 2
        return encode((self.seq+1) % 256, cmd, payload, flags)

    def preview(self):
        try: self.preview_text.set(self.build_frame().hex(" ").upper())
        except ValueError as exc: messagebox.showerror("协议参数", str(exc))

    def send_protocol(self):
        try:
            data = self.build_frame()
            self.send_bytes(data)
            self.seq = data[4]
            self.frames[self.seq] = (time.monotonic(), data[5])
            self.preview_text.set(data.hex(" ").upper())
        except ValueError as exc: messagebox.showerror("协议发送", str(exc))

    def refresh(self):
        for _ in range(100):
            try: kind, data = self.events.get_nowait()
            except queue.Empty: break
            if kind in ("rx", "tx"): self.traffic(kind.upper(), data)
            elif kind == "open": self.status.set("已连接独立串口 "+data)
            elif kind == "error": self.write("错误："+data)
            elif kind == "closed":
                self.worker = None; self.reset_stream(); self.status.set("独立串口已关闭")
        now = time.monotonic()
        for seq, (sent, cmd) in list(self.frames.items()):
            if now-sent > 2:
                self.write(f"协议 SEQ={seq} CMD=0x{cmd:02X} 等待对端响应超时；未重试，不能据此确认是否执行。")
                self.frames.pop(seq)

    def clear(self):
        self.output.configure(state="normal"); self.output.delete("1.0", "end"); self.output.configure(state="disabled")
        self.logs.clear()

    def save(self):
        path = filedialog.asksaveasfilename(defaultextension=".log", filetypes=[("串口日志", "*.log")])
        if path:
            try: Path(path).write_text("\n".join(self.logs)+"\n", encoding="utf-8")
            except OSError as exc: messagebox.showerror("保存日志", str(exc))

    def close(self):
        if self.worker:
            self.worker.quit.set(); self.worker.join(timeout=.4)
