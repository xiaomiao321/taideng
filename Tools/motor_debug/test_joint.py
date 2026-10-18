import unittest
from joint_model import Calibration, Nominal, millidegrees, encode_profile, decode_profile
from protocol import command, parse
from shell_commands import parse_shell

class JointTests(unittest.TestCase):
    def setUp(self):
        self.cal = Calibration(600, -30000, 3400, 90000, 1)

    def test_two_point_mapping_and_reverse(self):
        self.assertEqual(self.cal.validate().angle(2000), 30000)
        reverse = Calibration(3400, -30000, 600, 90000, -1).validate()
        self.assertEqual(reverse.angle(2000), 30000)
        self.assertEqual(millidegrees("-12.345"), -12345)

    def test_profile_round_trip(self):
        configs = {1: self.cal, 6: Calibration(800, 0, 3200, 120000, -1)}
        self.assertEqual(decode_profile(encode_profile(configs)), configs)
        for text in ('{"version":2,"joints":{}}', '{"version":1,"joints":{"7":{}}}'):
            with self.assertRaises(ValueError): decode_profile(text)

    def test_invalid_calibration(self):
        for values in ((0,0,3000,90000,1), (1000,0,1001,90000,1), (1000,90000,3000,0,1),
                       (1000,0,3000,90000,0), (1000,0,3000,90000,1,1001),
                       (1000,0,3000,90000,1,800,50000)):
            with self.assertRaises(ValueError): Calibration(*values).validate()
        for text in ("nan", "inf", "361", "1.0001"):
            with self.assertRaises(ValueError): millidegrees(text)

    def test_wire_and_telemetry(self):
        self.assertEqual(command("CAL", 20, 1, self.cal.wire()), "CAL 20 M1 600 -30000 3400 90000 1 800 2000\n")
        self.assertEqual(command("JMOVE", 21, 1, 45000, 10000), "JMOVE 21 M1 45000 10000\n")
        self.assertEqual(parse("JCAL 100 M1 600 -30000 3400 90000 1 800 2000"), ("JCAL", 100, 1, self.cal))
        self.assertEqual(parse("JSTATE 100 M1 1 1 2000 30000 45000 800 MOVE NONE")[6], 30000)
        self.assertEqual(parse("JRESULT 21 M1 REACHED"), ("JRESULT", 21, 1, "REACHED"))
        for kind in ("JGET", "JMOVE", "CAL"):
            with self.assertRaises(ValueError): command(kind, 1, 7)
        self.assertEqual(parse("JSTATE 100 M7 1 1 1000 0 0 0 IDLE NONE")[0], "BAD")
        self.assertEqual(parse_shell("angle M1 45").request, ("JMOVE", 1, 45000, 10000))
        self.assertEqual(parse_shell("joint M6").request[0:2], ("JGET", 6))
        with self.assertRaises(ValueError): parse_shell("angle M7 45")

    def test_nominal_matches_firmware(self):
        # 与 HostTest/joint_selftest.c 相同的输入，结果必须逐位一致。
        nominal = Nominal(2048, 90000, -1, 1, 800, 2000, 10000, 170000, 47000)
        cal = nominal.calibration()
        self.assertEqual((cal.raw_a, cal.raw_b, cal.angle_a, cal.angle_b), (3212, 884, 10000, 170000))
        self.assertEqual(cal.limits, (10000, 170000))
        self.assertEqual(nominal.endpoints(), (84, 4011))
        self.assertEqual(Nominal(2048, 90000, -1, 1).endpoints(), (87, 4008))  # 45kΩ 标称值
        self.assertTrue(89900 < cal.firmware_angle(2048) < 90100)
        for bad in (Nominal(2048, 90000, -1, 1, 800, 2000, 10000, 260000, 47000),   # 超出电气行程
                    Nominal(20, 90000, -1, 1, 800, 2000, 10000, 170000, 47000),     # 参考点低于端点
                    Nominal(2048, 90000, 0, 1, 800, 2000, 10000, 170000, 47000),    # 方向无效
                    Nominal(2048, 90000, -1, 1, 800, 2000, 10000, 170000, 500)):    # 阻值过小
            with self.assertRaises(ValueError): bad.calibration()

    def test_limits_separate_from_calibration(self):
        cal = Calibration(1000, 0, 3000, 180000, 1)
        self.assertEqual(cal.limits, (0, 180000))
        narrowed = cal.with_limits(20000, 160000).validate()
        self.assertEqual(narrowed.limits, (20000, 160000))
        self.assertEqual(narrowed.wire(), cal.wire())
        with self.assertRaises(ValueError): cal.with_limits(-100000, 100000).validate()  # 读数 1 约为 -89.9°
        with self.assertRaises(ValueError): cal.with_limits(50000, 50000).validate()
        # 旧的标定文件没有 limit 字段，仍能载入。
        old = '{"version":1,"joints":{"1":{"raw_a":600,"angle_a":-30000,"raw_b":3400,"angle_b":90000,"direction":1}}}'
        self.assertEqual(decode_profile(old)[1].limits, (-30000, 90000))

    def test_nominal_and_limit_wire(self):
        values = Nominal(2048, 90000, -1, 1, 800, 2000, 10000, 170000, 47000).wire()
        self.assertEqual(command("JNOM", 22, 1, values),
                         "JNOM 22 M1 2048 90000 -1 1 800 2000 10000 170000 47000\n")
        self.assertEqual(command("JLIM", 23, 2, (-5000, 120000)), "JLIM 23 M2 -5000 120000\n")
        with self.assertRaises(ValueError): command("JLIM", 24, 1, (120000, -5000))
        with self.assertRaises(ValueError): command("JNOM", 25, 7, values)
        self.assertEqual(parse("JLIM 100 M3 10000 170000"), ("JLIM", 100, 3, 10000, 170000))
        self.assertEqual(parse("JLIM 100 M3 170000 10000")[0], "BAD")
        self.assertEqual(parse_shell("jlim M2 -5 120").request, ("JLIM", 2, (-5000, 120000), 500))
        self.assertEqual(parse_shell("jnom M1 2048 90 down + 80 2 10 170 47").request[1:3], (1, values))
        self.assertEqual(parse_shell("jnom M1 2048 90 down + 80 2 10 170").request[2][-1], 0)
        for text in ("jnom M1 2048 90 sideways + 80 2 10 170", "jnom M1 2048 90 down + 80 2 10 260 47",
                     "jlim M7 0 90", "jnom M1 2048 90 down + 80 2 10 170 -3"):
            with self.assertRaises(ValueError): parse_shell(text)


if __name__ == "__main__": unittest.main()
