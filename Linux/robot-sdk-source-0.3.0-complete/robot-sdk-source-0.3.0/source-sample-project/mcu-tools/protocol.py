"""Reference implementation of the AItaid MCU serial protocol."""

from dataclasses import dataclass
import struct

SOF = b"\xA5\x5A"
VERSION = 0x02
MAX_PAYLOAD = 64

FLAG_RESPONSE = 0x01
FLAG_ACK_REQUIRED = 0x02
FLAG_ERROR = 0x04

CMD_PING = 0x01
CMD_SET_LIGHT = 0x10
CMD_SET_MOTION = 0x11
CMD_GET_STATUS = 0x20
CMD_STATUS = 0x21
CMD_SET_LIGHT_MODE = 0x30
CMD_SET_ARM_POSITION = 0x31
CMD_GET_EXTENDED_STATUS = 0x32
CMD_EXTENDED_STATUS = 0x33
CMD_HOME_ARM = 0x34
CMD_SET_LIGHT_ZONE = 0x35
CMD_SET_BUZZER = 0x36
CMD_SET_INDICATOR = 0x37
CMD_SOFT_SHUTDOWN = 0x38
CMD_ACK = 0x7F

RESULT_OK = 0x00
RESULT_INVALID_COMMAND = 0x01
RESULT_INVALID_LENGTH = 0x02
RESULT_INVALID_VALUE = 0x03

MOTION_STOP = 0x00
MOTION_FORWARD = 0x01
MOTION_BACKWARD = 0x02
MOTION_TURN_LEFT = 0x03
MOTION_TURN_RIGHT = 0x04
VALID_MOTIONS = {
    MOTION_STOP,
    MOTION_FORWARD,
    MOTION_BACKWARD,
    MOTION_TURN_LEFT,
    MOTION_TURN_RIGHT,
}


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = ((crc >> 1) ^ 0xA001) if crc & 1 else crc >> 1
    return crc & 0xFFFF


@dataclass(frozen=True)
class Frame:
    sequence: int
    command: int
    payload: bytes = b""
    flags: int = 0
    version: int = VERSION

    def encode(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD:
            raise ValueError("payload too large")
        body = struct.pack(
            "<BBBBH", self.version, self.flags, self.sequence, self.command, len(self.payload)
        ) + self.payload
        return SOF + body + struct.pack("<H", crc16_modbus(body))


class FrameDecoder:
    def __init__(self) -> None:
        self.buffer = bytearray()

    def feed(self, chunk: bytes) -> list[Frame]:
        self.buffer.extend(chunk)
        frames: list[Frame] = []
        while True:
            start = self.buffer.find(SOF)
            if start < 0:
                self.buffer[:] = self.buffer[-1:] if self.buffer.endswith(SOF[:1]) else b""
                break
            if start:
                del self.buffer[:start]
            if len(self.buffer) < 10:
                break
            version, flags, sequence, command, length = struct.unpack_from("<BBBBH", self.buffer, 2)
            if length > MAX_PAYLOAD:
                del self.buffer[0]
                continue
            frame_length = 10 + length
            if len(self.buffer) < frame_length:
                break
            expected_crc = struct.unpack_from("<H", self.buffer, frame_length - 2)[0]
            body = bytes(self.buffer[2 : frame_length - 2])
            if crc16_modbus(body) != expected_crc:
                del self.buffer[0]
                continue
            payload = bytes(self.buffer[8 : 8 + length])
            frames.append(Frame(sequence, command, payload, flags, version))
            del self.buffer[:frame_length]
        return frames


def ack(sequence: int, command: int, result: int = RESULT_OK, detail: int = 0) -> Frame:
    flags = FLAG_RESPONSE | (FLAG_ERROR if result else 0)
    return Frame(sequence, CMD_ACK, bytes((command, result, detail, 0)), flags)


def status(
    sequence: int,
    light_enabled: bool,
    brightness: int,
    motion: int,
    target_motion: int,
    motion_speed: int,
    target_speed: int,
    uptime_seconds: int,
    fault_flags: int = 0,
    status_flags: int = 0,
    protocol_error_count: int = 0,
) -> Frame:
    payload = struct.pack(
        "<BBBBBBBBIHH",
        int(light_enabled),
        brightness,
        motion,
        target_motion,
        motion_speed,
        target_speed,
        fault_flags,
        status_flags,
        uptime_seconds & 0xFFFFFFFF,
        protocol_error_count & 0xFFFF,
        0,
    )
    return Frame(sequence, CMD_STATUS, payload, FLAG_RESPONSE)


def hex_bytes(data: bytes) -> str:
    return " ".join(f"{value:02X}" for value in data)


def extended_status(sequence: int, command: int, selector: int, actual: int, target: int,
                    actual_speed: int = 0, target_speed: int = 0, fault_flags: int = 0) -> Frame:
    return Frame(sequence, CMD_EXTENDED_STATUS,
                 struct.pack("<BBBBiiii", command, selector, fault_flags, 0,
                             actual, target, actual_speed, target_speed), FLAG_RESPONSE)
