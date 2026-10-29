"""Android 主机替身：电脑经 USB 转 TTL 直连 MCU 的 USART1，测试 0x02 协议、底盘动作和关节扩展命令。

接线：USB 转 TTL 模块的 TX 接 PA10（USART1_RX），RX 接 PA9（USART1_TX），GND 相连，
不接 VCC；也就是板上原本接 RK3566 的那组串口。固件须按 APP_MODE_ANDROID 编译，
USART3 只出日志，可另开串口看。

    python link_test.py COM6 basic            PING、状态、灯光、STOP
    python link_test.py COM6 status           基本状态和 M1～M6 关节状态
    python link_test.py COM6 move M1 30       M1 转到 30°，按 SDK 规则判断是否到位
    python link_test.py COM6 move M1 -15 --rel  相对当前角度转 -15°
    python link_test.py COM6 watch M1 10      静止观察 10 秒，统计转速误报
    python link_test.py COM6 drive forward 30 2  底盘前进，速度 30%，2 秒后 STOP
    python link_test.py COM6 stop             STOP，清除原因已消失的故障
    python link_test.py COM6 silence 7        7 秒不发任何帧，验证 5 秒失联停车

drive 的动作是 forward、backward、left、right；左右转为原地转。
每 0.1 秒查询一次，命令之间的间隔远小于固件 5 秒的失联期限。
"""
import argparse
import struct
import sys
import time

VERSION = 0x02
FLAG_RESPONSE, FLAG_ACK_REQUIRED, FLAG_ERROR = 0x01, 0x02, 0x04
CMD_PING, CMD_SET_LIGHT, CMD_SET_MOTION = 0x01, 0x10, 0x11
CMD_GET_STATUS, CMD_STATUS = 0x20, 0x21
CMD_SET_ARM, CMD_GET_EXT, CMD_EXT, CMD_HOME = 0x31, 0x32, 0x33, 0x34
CMD_ACK = 0x7F
RESULTS = {0x00: "OK", 0x01: "INVALID_COMMAND", 0x02: "INVALID_LENGTH", 0x03: "INVALID_VALUE",
           0x04: "BUSY", 0x05: "HARDWARE", 0xFF: "INTERNAL"}
MOTIONS = {"stop": 0, "forward": 1, "backward": 2, "left": 3, "right": 4}
FAULT_NAMES = ("停滞", "超时", "方向错误", "越限", "电位器", "驱动", "底盘", "保留")
TOLERANCE_MDEG = 2000   # 与固件标定容差、SDK positionToleranceMilliDegrees 一致
OBSERVE_S = 10.0        # SDK 关节观察期限默认 10000ms


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def encode(seq, cmd, payload=b"", flags=FLAG_ACK_REQUIRED):
    body = bytes((VERSION, flags, seq, cmd)) + struct.pack("<H", len(payload)) + payload
    return b"\xA5\x5A" + body + struct.pack("<H", crc16(body))


class Decoder:
    """把串口字节流切成 (flags, seq, cmd, payload)，丢弃 CRC 错误的帧。"""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf.extend(data)
        frames = []
        while True:
            start = self.buf.find(b"\xA5\x5A")
            if start < 0:
                del self.buf[:-1]
                return frames
            del self.buf[:start]
            if len(self.buf) < 10:
                return frames
            length = self.buf[6] | self.buf[7] << 8
            if length > 64:
                del self.buf[:2]
                continue
            if len(self.buf) < 10 + length:
                return frames
            body = bytes(self.buf[2:8 + length])
            crc = self.buf[8 + length] | self.buf[9 + length] << 8
            if crc == crc16(body):
                frames.append((body[1], body[2], body[3], body[6:]))
                del self.buf[:10 + length]
            else:
                del self.buf[:2]


def faults_text(flags):
    names = [name for bit, name in enumerate(FAULT_NAMES) if flags >> bit & 1]
    return "无" if not names else f"0x{flags:02X} " + "、".join(names)


def parse_status(p):
    return {"light": p[0] & 1, "brightness": p[1], "motion": p[2], "target_motion": p[3],
            "speed": p[4], "target_speed": p[5], "faults": p[6],
            "uptime": struct.unpack_from("<I", p, 8)[0], "errors": struct.unpack_from("<H", p, 12)[0]}


def parse_ext(p):
    actual, target, speed, target_speed = struct.unpack_from("<iiii", p, 4)
    return {"command": p[0], "joint": p[1], "faults": p[2], "actual": actual, "target": target,
            "speed": speed, "target_speed": target_speed}


class Link:
    """一问一答；transport 需提供 write(bytes) 和 read(n)（超时返回空）。"""

    def __init__(self, transport):
        self.io = transport
        self.decoder = Decoder()
        self.seq = 0

    def request(self, cmd, payload=b"", flags=FLAG_ACK_REQUIRED, timeout=0.5):
        self.seq = self.seq % 255 + 1
        self.io.write(encode(self.seq, cmd, payload, flags))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for flags_, seq, cmd_, payload_ in self.decoder.feed(self.io.read(64)):
                if flags_ & FLAG_RESPONSE and seq == self.seq:
                    return cmd_, payload_
        raise TimeoutError(f"命令 0x{cmd:02X} 在 {timeout}s 内没有回复")

    def command(self, cmd, payload=b""):
        """发控制命令，返回 ACK 结果名。"""
        reply, p = self.request(cmd, payload)
        if reply != CMD_ACK or p[0] != cmd:
            raise ValueError(f"命令 0x{cmd:02X} 收到意外回复 0x{reply:02X}")
        return RESULTS.get(p[1], f"0x{p[1]:02X}") + (f" detail=0x{p[2]:02X}" if p[2] else "")

    def status(self):
        reply, p = self.request(CMD_GET_STATUS, flags=0)
        if reply != CMD_STATUS:
            raise ValueError(f"GET_STATUS 收到 0x{reply:02X}")
        return parse_status(p)

    def joint(self, joint_id):
        """查询关节扩展状态；不支持时返回 ACK 结果名。"""
        reply, p = self.request(CMD_GET_EXT, bytes((CMD_SET_ARM, joint_id, 0, 0)), flags=0)
        if reply == CMD_EXT:
            return parse_ext(p)
        return RESULTS.get(p[1], f"0x{p[1]:02X}")

    def move(self, joint_id, target, speed):
        return self.command(CMD_SET_ARM, struct.pack("<BBBBii", joint_id, 0, 0, 0, target, speed))

    def motion(self, code, speed):
        return self.command(CMD_SET_MOTION, bytes((code, speed, 0, 0)))

    def stop(self):
        return self.motion(0, 0)


def joint_text(j):
    if isinstance(j, str):
        return f"不可用：{j}"
    return (f"实际 {j['actual'] / 1000:8.2f}°  目标 {j['target'] / 1000:8.2f}°  "
            f"转速 {j['speed'] / 1000:6.1f}°/s  目标速度 {j['target_speed'] / 1000:5.1f}°/s  "
            f"故障 {faults_text(j['faults'])}")


def applied(link, joint_id, target, j):
    """按 SDK 规则判断到位：目标回读不变、误差在容差内、两个速度为 0、关节与全局故障为 0。"""
    return (not isinstance(j, str) and j["target"] == target and abs(j["actual"] - target) <= TOLERANCE_MDEG
            and j["speed"] == 0 and j["target_speed"] == 0 and j["faults"] == 0
            and link.status()["faults"] == 0)


def motion_applied(s, code, speed):
    """按 SDK 规则判断底盘动作到位：实际和目标动作、实际和目标速度都等于命令；STOP 不看故障。"""
    return (s["motion"] == code and s["target_motion"] == code and s["speed"] == speed
            and s["target_speed"] == speed and (code == 0 or s["faults"] == 0))


def motion_text(s):
    names = {code: name for name, code in MOTIONS.items()}
    return (f"动作 {names.get(s['motion'], s['motion'])}/{names.get(s['target_motion'], s['target_motion'])}  "
            f"速度 {s['speed']}%/{s['target_speed']}%  故障 {faults_text(s['faults'])}")


def run_basic(link):
    print("PING:", link.command(CMD_PING))
    s = link.status()
    print(f"状态：运行 {s['uptime']}s  协议错误 {s['errors']}  故障 {faults_text(s['faults'])}")
    print("开灯 30%:", link.command(CMD_SET_LIGHT, bytes((1, 30, 0, 0))))
    s = link.status()
    print(f"  回读：开关 {s['light']} 亮度 {s['brightness']}%")
    time.sleep(1)
    print("关灯:", link.command(CMD_SET_LIGHT, bytes((0, 30, 0, 0))))
    print("亮度 101 应被拒绝:", link.command(CMD_SET_LIGHT, bytes((1, 101, 0, 0))))
    print("前进速度 101 应被拒绝:", link.motion(1, 101))
    print("STOP:", link.stop())
    print("关节 7 应被拒绝:", link.move(7, 0, 20000))


def run_status(link):
    s = link.status()
    print(f"基本状态：运行 {s['uptime']}s  灯 {s['light']} {s['brightness']}%  "
          f"协议错误 {s['errors']}  底盘 {motion_text(s)}")
    for joint_id in range(1, 7):
        print(f"M{joint_id}  {joint_text(link.joint(joint_id))}")


def run_move(link, joint_id, text, speed, relative):
    start = link.joint(joint_id)
    if isinstance(start, str):
        sys.exit(f"M{joint_id} 不可用：{start}")
    target = round(float(text) * 1000) + (start["actual"] if relative else 0)
    print(f"M{joint_id} 从 {start['actual'] / 1000:.2f}° 转到 {target / 1000:.2f}°，速度上限 {speed / 1000:g}°/s")
    result = link.move(joint_id, target, speed)
    print("ACK:", result)
    if not result.startswith("OK"):
        return
    begin = time.monotonic()
    while time.monotonic() - begin < OBSERVE_S:
        j = link.joint(joint_id)
        print(f"{time.monotonic() - begin:5.1f}s  {joint_text(j)}")
        if applied(link, joint_id, target, j):
            print(f"APPLIED：{time.monotonic() - begin:.1f}s 到位，误差 {(j['actual'] - target) / 1000:+.2f}°")
            return
        time.sleep(0.1)
    print(f"STATE_TIMEOUT：{OBSERVE_S:g}s 内没有满足到位条件")


def run_watch(link, joint_id, seconds):
    count = moving = 0
    worst = 0
    begin = time.monotonic()
    while time.monotonic() - begin < seconds:
        j = link.joint(joint_id)
        if isinstance(j, str):
            sys.exit(f"M{joint_id} 不可用：{j}")
        count += 1
        moving += j["speed"] != 0
        worst = max(worst, abs(j["speed"]))
        time.sleep(0.1)
    print(f"M{joint_id} 静止观察 {count} 次：转速非零 {moving} 次，最大 {worst / 1000:.1f}°/s，"
          f"角度 {j['actual'] / 1000:.2f}°")


def wait_motion(link, code, speed, timeout=2.0):
    """等到状态回读满足到位规则，返回用时；超时返回 None。"""
    begin = time.monotonic()
    while time.monotonic() - begin < timeout:
        if motion_applied(link.status(), code, speed):
            return time.monotonic() - begin
        time.sleep(0.1)
    return None


def run_drive(link, name, speed, seconds):
    code = MOTIONS[name]
    print(f"底盘 {name} 速度 {speed}%，持续 {seconds:g}s")
    result = link.motion(code, speed)
    print("ACK:", result)
    if not result.startswith("OK"):
        return
    try:
        took = wait_motion(link, code, speed)
        print(f"APPLIED：{took:.1f}s" if took is not None else "STATE_TIMEOUT：状态回读与命令不一致")
        begin = time.monotonic()
        while time.monotonic() - begin < seconds:
            s = link.status()
            print(f"{time.monotonic() - begin:5.1f}s  {motion_text(s)}")
            time.sleep(0.2)
    finally:
        print("STOP:", link.stop())
    took = wait_motion(link, 0, 0)
    print(f"停止确认：{took:.1f}s" if took is not None else "停止确认超时")
    print(motion_text(link.status()))


def run_silence(link, seconds):
    print(f"{seconds:g}s 内不发任何帧；固件 5s 后应停掉全部电机（看电机或 USART3 日志）")
    time.sleep(seconds)
    run_status(link)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port")
    parser.add_argument("action", choices=("basic", "status", "move", "watch", "drive", "stop", "silence"))
    parser.add_argument("args", nargs="*")
    parser.add_argument("--speed", type=float, default=20, help="move 的速度上限，°/s，默认 20")
    parser.add_argument("--rel", action="store_true", help="move 的角度相对当前位置")
    a = parser.parse_args()
    import serial
    with serial.Serial(a.port, 115200, timeout=0.02) as port:
        port.reset_input_buffer()
        link = Link(port)
        if a.action == "basic":
            run_basic(link)
        elif a.action == "status":
            run_status(link)
        elif a.action == "stop":
            print("STOP:", link.stop())
            run_status(link)
        elif a.action == "silence":
            run_silence(link, float(a.args[0]) if a.args else 7)
        elif a.action == "drive":
            if not 2 <= len(a.args) <= 3 or a.args[0] not in MOTIONS or a.args[0] == "stop":
                sys.exit("用法：drive forward 30 2，动作为 forward、backward、left、right")
            speed = int(a.args[1])
            if not 0 <= speed <= 100:
                sys.exit("速度为 0～100")
            run_drive(link, a.args[0], speed, float(a.args[2]) if len(a.args) > 2 else 1)
        else:
            if not a.args or a.args[0].upper() not in tuple(f"M{i}" for i in range(1, 7)):
                sys.exit("需要关节名 M1～M6")
            joint_id = int(a.args[0][1])
            if a.action == "move":
                if len(a.args) != 2:
                    sys.exit("用法：move M1 30 或 move M1 -15 --rel")
                run_move(link, joint_id, a.args[1], round(a.speed * 1000), a.rel)
            else:
                run_watch(link, joint_id, float(a.args[1]) if len(a.args) > 1 else 10)


if __name__ == "__main__":
    main()
