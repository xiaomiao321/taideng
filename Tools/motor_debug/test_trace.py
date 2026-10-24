import csv
import os
import tempfile
import unittest
from protocol import command, parse
from shell_commands import parse_shell
from trace_recorder import TraceRecorder, HEADER


class TraceTests(unittest.TestCase):
    def test_commands(self):
        self.assertEqual(command("JTUNE", 3, 1, (1, 30, 300, 50)), "JTUNE 3 M1 1 30 300 50\n")
        self.assertEqual(command("JTRACE", 4, 2, 1), "JTRACE 4 M2 1\n")
        self.assertEqual(command("MPWM", 5, None), "MPWM 5\n")
        self.assertEqual(command("MPWM", 6, 5, 1), "MPWM 6 5 1\n")
        for bad in ((1, 30, -300, 50), (2, 30, 300, 50), (1, 1001, 300, 50), (1, 30, 99, 50), (1, 30, 300, 201)):
            with self.assertRaises(ValueError): command("JTUNE", 1, 1, bad)
        with self.assertRaises(ValueError): command("JTUNE", 1, 7, (1, 30, 300, 50))
        with self.assertRaises(ValueError): command("JTRACE", 1, 1, 2)
        with self.assertRaises(ValueError): command("MPWM", 1, 3, 1)
        with self.assertRaises(ValueError): command("MPWM", 1, 5, 2)

    def test_shell(self):
        self.assertEqual(parse_shell("tune M1 p 3 30 50").request, ("JTUNE", 1, (1, 30, 300, 50), 500))
        self.assertEqual(parse_shell("tune m2 bang").request, ("JTUNE", 2, (0, 30, 500, 50), 500))
        self.assertEqual(parse_shell("trace M1 on").request, ("JTRACE", 1, 1, 500))
        self.assertEqual(parse_shell("trace M1 OFF").request, ("JTRACE", 1, 0, 500))
        self.assertEqual(parse_shell("pwm").request, ("MPWM", None, 800, 500))
        self.assertEqual(parse_shell("pwm 5 brake").request, ("MPWM", 5, 1, 500))
        self.assertEqual(parse_shell("pwm 20 coast").request, ("MPWM", 20, 0, 500))
        for text in ("tune M1 p 3 5 50", "tune M7 bang", "trace M1 maybe", "pwm 3 brake", "pwm 5 hold"):
            with self.assertRaises(ValueError, msg=text): parse_shell(text)

    def test_parse(self):
        self.assertEqual(parse("JT 1234 M1 2048 6 90123 150000 -450 1 MOVE"),
                         ("JT", 1234, 1, 2048, 6, 90123, 150000, -450, True, "MOVE"))
        self.assertEqual(parse("JTUNE 10 M3 1 30 300 50"), ("JTUNE", 10, 3, 1, 30, 300, 50))
        self.assertEqual(parse("MPWM 5 1"), ("MPWM", 5, True))
        self.assertEqual(parse("JT 1 M1 4096 0 0 0 0 0 IDLE")[0], "BAD")
        self.assertEqual(parse("JT 1 M1 2048 0 0 0 0 2 IDLE")[0], "BAD")

    def test_recorder(self):
        now = [100.0]
        with tempfile.TemporaryDirectory() as folder:
            rec = TraceRecorder(folder, clock=lambda: now[0])
            first = rec.add(parse("JT 10 M1 2048 6 90123 150000 -450 0 MOVE"))
            self.assertTrue(first and os.path.exists(first))
            now[0] += 0.01
            self.assertIsNone(rec.add(parse("JT 20 M1 2050 4 90300 150000 0 1 MOVE")))
            self.assertIsNone(rec.idle())
            now[0] += 2
            self.assertEqual(rec.idle(), (first, 2))
            with open(first, encoding="utf-8") as f:
                rows = list(csv.reader(f))
            self.assertEqual(tuple(rows[0]), HEADER)
            self.assertEqual(rows[1][1:], ["10", "M1", "2048", "6", "90.123", "150.000", "-45", "0", "MOVE"])
            self.assertEqual(rows[2][8], "1")
            # 切换关节立即换新文件。
            second = rec.add(parse("JT 30 M1 2048 6 90123 150000 0 0 IDLE"))
            now[0] += 0.01
            third = rec.add(parse("JT 40 M2 2048 6 90123 150000 0 0 IDLE"))
            self.assertTrue(second and third and third.endswith("_M2.csv"))
            rec.close()


if __name__ == "__main__":
    unittest.main()
