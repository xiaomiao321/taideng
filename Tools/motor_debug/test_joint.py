import unittest
from joint_model import Calibration, millidegrees, encode_profile, decode_profile
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

if __name__ == "__main__": unittest.main()
