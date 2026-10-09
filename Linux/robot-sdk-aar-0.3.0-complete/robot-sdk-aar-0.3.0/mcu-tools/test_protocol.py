import unittest

from protocol import (
    CMD_SET_LIGHT,
    CMD_SET_MOTION,
    CMD_STATUS,
    Frame,
    FrameDecoder,
    crc16_modbus,
    MOTION_FORWARD,
    status,
)


class ProtocolTest(unittest.TestCase):
    def test_crc_reference_vector(self) -> None:
        self.assertEqual(0x4B37, crc16_modbus(b"123456789"))

    def test_fragmented_frame_round_trip(self) -> None:
        source = Frame(42, CMD_SET_LIGHT, bytes((1, 73, 0, 0)), flags=2)
        encoded = source.encode()
        decoder = FrameDecoder()
        self.assertEqual([], decoder.feed(encoded[:5]))
        self.assertEqual([], decoder.feed(encoded[5:9]))
        self.assertEqual([source], decoder.feed(encoded[9:]))

    def test_noise_bad_crc_and_resynchronization(self) -> None:
        bad = bytearray(Frame(1, 0x20).encode())
        bad[-1] ^= 0x55
        valid = Frame(2, CMD_SET_LIGHT, bytes((0, 45, 0, 0)), flags=2)
        frames = FrameDecoder().feed(b"noise" + bad + valid.encode())
        self.assertEqual([valid], frames)

    def test_status_payload_is_16_bytes(self) -> None:
        frame = status(
            7,
            True,
            82,
            MOTION_FORWARD,
            MOTION_FORWARD,
            75,
            75,
            0x12345678,
            fault_flags=8,
        )
        self.assertEqual(CMD_STATUS, frame.command)
        self.assertEqual(16, len(frame.payload))
        self.assertEqual(bytes((MOTION_FORWARD, MOTION_FORWARD, 75, 75, 8, 0)), frame.payload[2:8])
        self.assertEqual(b"\x78\x56\x34\x12", frame.payload[8:12])

    def test_macro_motion_command_has_action_speed_and_reserved_bytes(self) -> None:
        frame = Frame(3, CMD_SET_MOTION, bytes((MOTION_FORWARD, 80, 0, 0)), flags=2)
        decoded = FrameDecoder().feed(frame.encode())
        self.assertEqual([frame], decoded)


if __name__ == "__main__":
    unittest.main()
