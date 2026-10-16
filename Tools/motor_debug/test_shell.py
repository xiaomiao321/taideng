import unittest
from shell_commands import parse_shell, complete

class ShellTests(unittest.TestCase):
    def test_translation(self):
        cases = {
            "motor M1 80 500": ("RUN", 1, 800, 500),
            "motor m6 -80.5 0": ("RUN", 6, -805, 0),
            "lamp up1 50 1000": ("TEST", "LAMP_UP1", 500, 1000),
            "buzzer 2700 1000": ("TEST", "BUZZER", 2700, 1000),
            "power": ("TEST", "PWR", 0, 200),
            "led": ("TEST", "LED", 0, 1000),
            "log": ("TEST", "LOG", 0, 500),
            "adc test": ("TEST", "ADC6", 0, 500),
            "key test": ("TEST", "KEY", 0, 15000),
            "test MOTOR_M1_BI": ("TEST", "MOTOR_M1_BI", 800, 500),
            "test LAMP_DOWN3 50 2000": ("TEST", "LAMP_DOWN3", 500, 2000),
            "pass": ("CONFIRM", "PASS", 800, 500),
            "fail": ("CONFIRM", "FAIL", 800, 500),
            "stop": ("STOP", 1, 800, 500),
        }
        for text, expected in cases.items():
            self.assertEqual(parse_shell(text).request, expected, text)

    def test_local_commands(self):
        for name in ("help", "clear", "history", "adc", "key", "status"):
            self.assertEqual(parse_shell(name).action, name)
        self.assertEqual(parse_shell("test").action, "list")
        self.assertEqual(parse_shell("watch ON").argument, "on")
        self.assertEqual(parse_shell(" # comment").action, "empty")

    def test_reject_invalid_or_multiple_commands(self):
        for text in ("motor M1 0 500", "motor M9 80 500", "motor M1 nan 500",
                     "motor M1 inf 500", "motor M1 80.01 500", "motor M1 800 500",
                     "motor M1 80 -1", "motor M1 80 60001", "lamp UP5 50 1000",
                     "power 10001", "buzzer 1 1000", "test MOTOR_M1_FI 80 5001",
                     "stop\nmotor M1 80 0", "stop; motor M1 80 0", "rm -rf /", 'motor "'):
            with self.assertRaises(ValueError, msg=text):
                parse_shell(text)

    def test_completion(self):
        self.assertEqual(complete("mot"), ["motor"])
        self.assertEqual(complete("motor m1"), ["motor M1"])
        self.assertEqual(complete("lamp DOWN"), ["lamp DOWN1", "lamp DOWN2", "lamp DOWN3"])
        self.assertIn("test MOTOR_M8_BI", complete("test MOTOR_M8"))
        self.assertEqual(complete("watch o"), ["watch on", "watch off"])
        self.assertEqual(complete("motor M1 8"), [])

if __name__ == "__main__":
    unittest.main()
