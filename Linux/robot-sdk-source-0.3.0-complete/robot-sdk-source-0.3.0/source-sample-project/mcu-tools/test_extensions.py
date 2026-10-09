"""No hardware IO: extension fixtures, framing, and ACK-vs-application behavior."""
import struct
import unittest

from mcu_simulator import McuSimulator
from protocol import (Frame, FrameDecoder, FLAG_ACK_REQUIRED, CMD_SET_ARM_POSITION,
                      CMD_SET_LIGHT_MODE, CMD_GET_EXTENDED_STATUS, CMD_EXTENDED_STATUS,
                      CMD_HOME_ARM, CMD_SET_LIGHT_ZONE, CMD_SET_BUZZER, CMD_SET_INDICATOR,
                      CMD_SOFT_SHUTDOWN, CMD_SET_MOTION, CMD_GET_STATUS, RESULT_INVALID_COMMAND,
                      RESULT_INVALID_VALUE, MOTION_FORWARD)


class ExtensionTests(unittest.TestCase):
    def frame(self, command, payload):
        return Frame(42, command, payload, FLAG_ACK_REQUIRED)

    def test_legacy_refuses_every_reserved_extension(self):
        sim = McuSimulator()
        for command in (CMD_SET_ARM_POSITION, CMD_SET_LIGHT_MODE, CMD_GET_EXTENDED_STATUS,
                        CMD_HOME_ARM, CMD_SET_LIGHT_ZONE, CMD_SET_BUZZER,
                        CMD_SET_INDICATOR, CMD_SOFT_SHUTDOWN):
            self.assertEqual(RESULT_INVALID_COMMAND, sim.handle(self.frame(command, b"\0" * 4))[0].payload[1])
        self.assertFalse(sim.shutdown_requested)

    def test_joint_payload_signed_bounds_and_crc_fragmentation(self):
        sim = McuSimulator(extensions=True)
        frame = self.frame(CMD_SET_ARM_POSITION, struct.pack("<Bxxxii", 3, -20000, 10000))
        encoded = frame.encode()
        decoder = FrameDecoder()
        self.assertEqual([], decoder.feed(encoded[:7]))
        self.assertEqual([frame], decoder.feed(encoded[7:]))
        damaged = bytearray(encoded)
        damaged[12] ^= 1
        self.assertEqual([frame], FrameDecoder().feed(damaged + encoded))
        self.assertEqual(0, sim.handle(frame)[0].payload[1])
        wrong_range = self.frame(CMD_SET_ARM_POSITION, struct.pack("<Bxxxii", 3, 90000, 10000))
        self.assertEqual(RESULT_INVALID_VALUE, sim.handle(wrong_range)[0].payload[1])
        query = self.frame(CMD_GET_EXTENDED_STATUS, bytes((CMD_SET_ARM_POSITION, 3, 0, 0)))
        response = sim.handle(query)[0]
        self.assertEqual(CMD_EXTENDED_STATUS, response.command)
        self.assertEqual((-20000, -20000, 0, 0), struct.unpack_from("<iiii", response.payload, 4))

    def test_extension_ack_only_reports_target_without_actual(self):
        sim = McuSimulator(extensions=True, ack_only=True)
        frame = self.frame(CMD_SET_LIGHT_MODE, bytes((7, 60, 0, 0)))
        self.assertEqual(0, sim.handle(frame)[0].payload[1])
        response = sim.handle(self.frame(CMD_GET_EXTENDED_STATUS, bytes((CMD_SET_LIGHT_MODE, 7, 0, 0))))[0]
        self.assertEqual((0, 7, 0, 60), struct.unpack_from("<iiii", response.payload, 4))

    def test_basic_ack_only_does_not_create_motion(self):
        sim = McuSimulator(ack_only=True)
        response = sim.handle(self.frame(CMD_SET_MOTION, bytes((MOTION_FORWARD, 30, 0, 0))))
        self.assertEqual(0, response[0].payload[1])
        self.assertEqual(0, response[1].payload[2])
        self.assertEqual(0, sim.handle(self.frame(CMD_GET_STATUS, b""))[0].payload[2])

    def test_other_extension_validation_and_shutdown_are_only_simulated(self):
        sim = McuSimulator(extensions=True)
        for command, payload in ((CMD_HOME_ARM, bytes((3, 0, 0, 0))),
                                 (CMD_SET_LIGHT_ZONE, bytes((1, 1, 60, 0))),
                                 (CMD_SET_BUZZER, bytes((1, 40, 0, 0))),
                                 (CMD_SET_INDICATOR, bytes((2, 1, 0, 0)))):
            self.assertEqual(0, sim.handle(self.frame(command, payload))[0].payload[1])
        self.assertEqual(RESULT_INVALID_VALUE,
                         sim.handle(self.frame(CMD_SET_LIGHT_ZONE, bytes((9, 1, 60, 0))))[0].payload[1])
        self.assertEqual(0, sim.handle(self.frame(CMD_SOFT_SHUTDOWN, b"SD\x01\x00"))[0].payload[1])
        self.assertTrue(sim.shutdown_requested)  # A fixture flag; no real power is touched.

    def test_read_only_query_returns_initial_joint_without_actuation(self):
        sim = McuSimulator(extensions=True)
        query = self.frame(CMD_GET_EXTENDED_STATUS, bytes((CMD_SET_ARM_POSITION, 3, 0, 0)))
        response = sim.handle(query)[0]
        self.assertEqual(CMD_EXTENDED_STATUS, response.command)
        self.assertEqual((0, 0, 0, 0), struct.unpack_from("<iiii", response.payload, 4))
        self.assertFalse(sim.shutdown_requested)


if __name__ == "__main__":
    unittest.main()
