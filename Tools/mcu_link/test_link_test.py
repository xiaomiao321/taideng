import struct
import time
import unittest

from link_test import (CMD_ACK, CMD_EXT, CMD_GET_EXT, CMD_SET_ARM, CMD_STATUS, Decoder, Link, applied,
                       crc16, encode, faults_text, motion_applied)


class FakeMcu:
    """最小的 MCU 替身：只回 GET_STATUS、GET_EXTENDED_STATUS 和关节命令的 ACK。"""

    def __init__(self):
        self.out = bytearray()
        self.decoder = Decoder()
        self.joint = [0, 30000, 30000, 0, 0, 0]  # 关节号 实际 目标 转速 目标速度 故障
        self.faults = 0
        self.motion = (0, 0)  # 动作 速度

    def write(self, data):
        for flags, seq, cmd, p in self.decoder.feed(data):
            if cmd == 0x20:
                m, v = self.motion
                payload = bytes((0, 0, m, m, v, v, self.faults, 0)) + struct.pack("<IH", 5, 0) + bytes(2)
                self.out += encode(seq, CMD_STATUS, payload, 0x01)
            elif cmd == CMD_GET_EXT:
                j = self.joint
                payload = bytes((p[0], p[1], j[5], 0)) + struct.pack("<iiii", *j[1:5])
                self.out += encode(seq, CMD_EXT, payload, 0x01)
            else:
                if cmd == 0x11:
                    self.motion = (p[0], p[1])
                self.out += encode(seq, CMD_ACK, bytes((cmd, 0, 0, 0)), 0x01)

    def read(self, n):
        data, self.out = bytes(self.out[:n]), self.out[n:]
        return data


class LinkTest(unittest.TestCase):
    def test_crc_matches_modbus_vector(self):
        self.assertEqual(crc16(b"123456789"), 0x4B37)

    def test_encode_matches_firmware_layout(self):
        frame = encode(0x40, CMD_SET_ARM, struct.pack("<BBBBii", 3, 0, 0, 0, -30000, 20000))
        self.assertEqual(frame[:8], bytes((0xA5, 0x5A, 0x02, 0x02, 0x40, 0x31, 12, 0)))
        self.assertEqual(frame[12:16], struct.pack("<i", -30000))
        self.assertEqual(len(frame), 22)

    def test_decoder_skips_noise_and_bad_crc(self):
        good = encode(7, CMD_ACK, bytes((0x10, 0, 0, 0)), 0x01)
        bad = bytearray(good)
        bad[-1] ^= 0xFF
        frames = Decoder().feed(b"\x00\xA5" + bytes(bad) + good[:5])
        self.assertEqual(frames, [])
        d = Decoder()
        d.feed(b"\x13\x37" + good[:5])
        self.assertEqual(d.feed(good[5:]), [(0x01, 7, CMD_ACK, bytes((0x10, 0, 0, 0)))])

    def test_queries_and_applied_rule(self):
        mcu = FakeMcu()
        link = Link(mcu)
        self.assertEqual(link.move(1, 30000, 20000), "OK")
        j = link.joint(1)
        self.assertEqual((j["actual"], j["target"]), (30000, 30000))
        self.assertTrue(applied(link, 1, 30000, j))
        mcu.joint[1] = 32001  # 超出 2° 容差
        self.assertFalse(applied(link, 1, 30000, link.joint(1)))
        mcu.joint[1] = 30500
        mcu.joint[3] = 2500   # 还在转
        self.assertFalse(applied(link, 1, 30000, link.joint(1)))
        mcu.joint[3] = 0
        mcu.faults = 0x10     # 全局故障
        self.assertFalse(applied(link, 1, 30000, link.joint(1)))

    def test_motion_applied_rule(self):
        mcu = FakeMcu()
        link = Link(mcu)
        self.assertEqual(link.motion(3, 40), "OK")
        self.assertTrue(motion_applied(link.status(), 3, 40))
        self.assertFalse(motion_applied(link.status(), 3, 41))
        mcu.faults = 0x40
        self.assertFalse(motion_applied(link.status(), 3, 40))
        self.assertEqual(link.stop(), "OK")
        self.assertTrue(motion_applied(link.status(), 0, 0))  # STOP 只确认停住，不要求无故障

    def test_timeout_when_silent(self):
        class Silent:
            def write(self, data):
                pass

            def read(self, n):
                time.sleep(0.01)
                return b""
        with self.assertRaises(TimeoutError):
            Link(Silent()).request(0x01, timeout=0.05)

    def test_faults_text(self):
        self.assertEqual(faults_text(0), "无")
        self.assertEqual(faults_text(0x11), "0x11 停滞、电位器")


if __name__ == "__main__":
    unittest.main()
