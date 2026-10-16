"""SDK protocol v2 framing, matching Linux/…/MCU通信协议.md.

This module only encodes/decodes frames; it does not claim device support.
"""
import struct


def crc16(data):
    crc = 0xffff
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
    return crc


def encode(seq, cmd, payload=b"", flags=2):
    if any(type(v) is not int or not 0 <= v <= 255 for v in (seq, cmd, flags)) or len(payload) > 64:
        raise ValueError("命令、序号、标志须为 0～255；协议负载最多 64 字节")
    body = struct.pack("<BBBBH", 2, flags, seq, cmd, len(payload)) + payload
    return b"\xa5\x5a" + body + struct.pack("<H", crc16(body))


class Decoder:
    def __init__(self):
        self.buffer = bytearray()
        self.errors = 0

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while True:
            start = self.buffer.find(b"\xa5\x5a")
            if start < 0:
                self.buffer[:] = self.buffer[-1:] if self.buffer.endswith(b"\xa5") else b""
                break
            if start: del self.buffer[:start]
            if len(self.buffer) < 8: break
            version, flags, seq, cmd, size = struct.unpack_from("<BBBBH", self.buffer, 2)
            if version != 2 or size > 64:
                self.errors += 1; del self.buffer[0]; continue
            if len(self.buffer) < size+10: break
            raw = bytes(self.buffer[:size+10])
            if crc16(raw[2:-2]) != int.from_bytes(raw[-2:], "little"):
                self.errors += 1; del self.buffer[0]; continue
            del self.buffer[:size+10]
            result.append((seq, cmd, flags, raw[8:-2]))
        return result


def describe(frame):
    seq, cmd, flags, payload = frame
    prefix = f"SEQ={seq} CMD=0x{cmd:02X} FLAGS=0x{flags:02X} "
    if cmd == 0x7f and len(payload) == 4:
        labels = {0: "接受成功", 1: "未知命令", 2: "长度错误", 3: "参数错误", 4: "忙", 5: "硬件故障", 255: "内部错误"}
        return prefix + f"ACK(0x{payload[0]:02X}) {labels.get(payload[1], '未知结果')} DETAIL={payload[2]}；ACK 不等于动作完成"
    if cmd == 0x21 and len(payload) == 16:
        return prefix + (f"灯={payload[0]&1} 亮度={payload[1]}% 动作={payload[2]}→{payload[3]} "
                         f"速度={payload[4]}→{payload[5]} 故障=0x{payload[6]:02X} "
                         f"运行={int.from_bytes(payload[8:12], 'little')}s 协议错误={int.from_bytes(payload[12:14], 'little')}")
    return prefix + "PAYLOAD=" + payload.hex(" ").upper()
