"""PC-side MCU simulator for a crossed pair of USB-UART adapters."""

import argparse
import time

from protocol import (
    CMD_GET_STATUS,
    CMD_PING,
    CMD_SET_LIGHT,
    CMD_SET_MOTION,
    CMD_SET_LIGHT_MODE, CMD_SET_ARM_POSITION, CMD_GET_EXTENDED_STATUS,
    CMD_HOME_ARM, CMD_SET_LIGHT_ZONE, CMD_SET_BUZZER, CMD_SET_INDICATOR, CMD_SOFT_SHUTDOWN,
    FLAG_ACK_REQUIRED,
    Frame,
    FrameDecoder,
    RESULT_INVALID_COMMAND,
    RESULT_INVALID_LENGTH,
    RESULT_INVALID_VALUE,
    MOTION_STOP,
    VALID_MOTIONS,
    ack,
    hex_bytes,
    status,
    extended_status,
    VERSION,
)
import struct

# Test fixture only. These ranges/IDs are not measurements of any connected MCU.
SIMULATED_CAPABILITIES = {
    "firmwareDescriptorId": "aitaid-simulator-extension-v1",
    "protocolVersion": VERSION,
    "lightModes": [7], "lightZoneIds": [1], "buzzerPatternIds": [0, 1], "indicatorIds": [2],
    "supportsSoftShutdown": True,
    "joints": [{"jointId": 3, "minPositionMilliDegrees": -45000, "maxPositionMilliDegrees": 80000,
                "maxSpeedMilliDegreesPerSecond": 20000, "homePositionMilliDegrees": 0}],
}


class McuSimulator:
    def __init__(self, extensions: bool = False, ack_only: bool = False) -> None:
        self.started_at = time.monotonic()
        self.light_enabled = False
        self.brightness = 50
        self.motion = MOTION_STOP
        self.motion_speed = 0
        self.protocol_errors = 0
        self.extensions = extensions
        self.ack_only = ack_only
        self.ext_state: dict[tuple[int, int], tuple[int, int, int, int]] = {}
        if extensions:
            self.ext_state = {
                (CMD_SET_ARM_POSITION, 3): (0, 0, 0, 0), (CMD_HOME_ARM, 3): (0, 0, 0, 0),
                (CMD_SET_LIGHT_MODE, 7): (0, 0, 50, 50), (CMD_SET_LIGHT_ZONE, 1): (0, 0, 50, 50),
                (CMD_SET_BUZZER, 0): (0, 0, 0, 0), (CMD_SET_BUZZER, 1): (0, 0, 0, 0),
                (CMD_SET_INDICATOR, 2): (0, 0, 0, 0),
            }
        self.shutdown_requested = False

    def handle(self, frame: Frame) -> list[Frame]:
        if frame.version != VERSION:
            return [ack(frame.sequence, frame.command, RESULT_INVALID_COMMAND)]
        if frame.command in {CMD_SET_LIGHT_MODE, CMD_SET_ARM_POSITION, CMD_GET_EXTENDED_STATUS,
                             CMD_HOME_ARM, CMD_SET_LIGHT_ZONE, CMD_SET_BUZZER,
                             CMD_SET_INDICATOR, CMD_SOFT_SHUTDOWN}:
            return self.handle_extension(frame)
        if frame.command == CMD_SET_LIGHT:
            if len(frame.payload) != 4:
                return [ack(frame.sequence, frame.command, RESULT_INVALID_LENGTH)]
            if frame.payload[1] > 100 or frame.payload[0] & 0xFE:
                return [ack(frame.sequence, frame.command, RESULT_INVALID_VALUE)]
            if not self.ack_only:
                self.light_enabled = bool(frame.payload[0] & 1)
                self.brightness = frame.payload[1]
            return [ack(frame.sequence, frame.command), self.make_status(frame.sequence)]

        if frame.command == CMD_SET_MOTION:
            if len(frame.payload) != 4:
                return [ack(frame.sequence, frame.command, RESULT_INVALID_LENGTH)]
            motion, speed, control_flags, reserved = frame.payload
            if (
                motion not in VALID_MOTIONS
                or speed > 100
                or control_flags != 0
                or reserved != 0
                or (motion == MOTION_STOP and speed != 0)
            ):
                return [ack(frame.sequence, frame.command, RESULT_INVALID_VALUE)]
            if not self.ack_only:
                self.motion = motion
                self.motion_speed = speed
            return [ack(frame.sequence, frame.command), self.make_status(frame.sequence)]

        if frame.command == CMD_GET_STATUS:
            if frame.payload:
                return [ack(frame.sequence, frame.command, RESULT_INVALID_LENGTH)]
            return [self.make_status(frame.sequence)]

        if frame.command == CMD_PING:
            return [ack(frame.sequence, frame.command)]

        if frame.flags & FLAG_ACK_REQUIRED:
            return [ack(frame.sequence, frame.command, RESULT_INVALID_COMMAND)]
        return []

    def handle_extension(self, frame: Frame) -> list[Frame]:
        if not self.extensions:
            return [ack(frame.sequence, frame.command, RESULT_INVALID_COMMAND)]
        p = frame.payload
        command = frame.command
        expected = 12 if command == CMD_SET_ARM_POSITION else 4
        if len(p) != expected:
            return [ack(frame.sequence, command, RESULT_INVALID_LENGTH)]
        selector = p[0]
        value = speed = 0
        if command == CMD_GET_EXTENDED_STATUS:
            if p[2:] != b"\x00\x00":
                return [ack(frame.sequence, command, RESULT_INVALID_VALUE)]
            values = self.ext_state.get((p[0], p[1]))
            if values is None:
                return [ack(frame.sequence, command, RESULT_INVALID_VALUE)]
            return [extended_status(frame.sequence, p[0], p[1], *values)]
        if command == CMD_SET_ARM_POSITION:
            value, requested_speed = struct.unpack_from("<ii", p, 4)
            valid = selector == 3 and p[1:4] == b"\x00\x00\x00" and -45000 <= value <= 80000 and 0 < requested_speed <= 20000
        elif command == CMD_SET_LIGHT_MODE:
            valid = selector == 7 and p[1] <= 100 and p[2:] == b"\x00\x00"
            value, speed = selector, p[1]
        elif command == CMD_HOME_ARM:
            valid = selector == 3 and p[1:] == b"\x00\x00\x00"
        elif command == CMD_SET_LIGHT_ZONE:
            valid = selector == 1 and p[1] in (0, 1) and p[2] <= 100 and p[3] == 0
            value, speed = p[1], p[2]
        elif command == CMD_SET_BUZZER:
            valid = selector in (0, 1) and p[1] <= 100 and p[2:] == b"\x00\x00"
            value, speed = selector, p[1]
        elif command == CMD_SET_INDICATOR:
            valid = selector == 2 and p[1] in (0, 1) and p[2:] == b"\x00\x00"
            value = p[1]
        elif command == CMD_SOFT_SHUTDOWN:
            valid = p == b"SD\x01\x00"
            if valid and not self.ack_only:
                self.motion = MOTION_STOP
                self.motion_speed = 0
                self.shutdown_requested = True
            return [ack(frame.sequence, command, 0 if valid else RESULT_INVALID_VALUE)]
        else:
            valid = False
        if not valid:
            return [ack(frame.sequence, command, RESULT_INVALID_VALUE)]
        self.ext_state[(command, selector)] = (0, value, 0, speed) if self.ack_only else (value, value, speed, speed)
        if command in (CMD_SET_ARM_POSITION, CMD_HOME_ARM):
            self.ext_state[(CMD_SET_ARM_POSITION, selector)] = self.ext_state[(command, selector)]
            self.ext_state[(CMD_HOME_ARM, selector)] = self.ext_state[(command, selector)]
        return [ack(frame.sequence, command)]

    def make_status(self, sequence: int) -> Frame:
        return status(
            sequence=sequence,
            light_enabled=self.light_enabled,
            brightness=self.brightness,
            motion=self.motion,
            target_motion=self.motion,
            motion_speed=self.motion_speed,
            target_speed=self.motion_speed,
            uptime_seconds=int(time.monotonic() - self.started_at),
            protocol_error_count=self.protocol_errors,
        )


def main() -> None:
    import serial
    parser = argparse.ArgumentParser(description="AItaid MCU serial simulator")
    parser.add_argument("--port", required=True, help="PC USB-UART port, for example COM7")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--extensions", action="store_true", help="Enable simulated extension-v1 fixture, not real hardware support")
    parser.add_argument("--ack-only", action="store_true", help="Accept commands without applying target state")
    args = parser.parse_args()

    decoder = FrameDecoder()
    simulator = McuSimulator(args.extensions, args.ack_only)
    with serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=1.0) as uart:
        print(f"MCU simulator listening on {args.port} @ {args.baud} baud")
        while True:
            chunk = uart.read(max(uart.in_waiting, 1))
            for frame in decoder.feed(chunk):
                print(f"RX seq={frame.sequence:03d} cmd=0x{frame.command:02X} {hex_bytes(frame.payload)}")
                for response in simulator.handle(frame):
                    encoded = response.encode()
                    uart.write(encoded)
                    uart.flush()
                    print(
                        f"TX seq={response.sequence:03d} cmd=0x{response.command:02X} "
                        f"{hex_bytes(response.payload)}"
                    )


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nSimulator stopped")
