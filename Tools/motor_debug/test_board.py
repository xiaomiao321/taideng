import re
import unittest
from pathlib import Path
from board_catalog import CASES, test_parameters
from protocol import command, parse
from board_panel import BoardPanel

class BoardTests(unittest.TestCase):
    def test_catalog_matches_firmware_bringup_table(self):
        root = Path(__file__).resolve().parents[2]
        src = (root / "MCU/software/test_bringup/Tests/Src/bringup_cases.c").read_text(encoding="utf-8")
        table = src.split("g_bringup_cases[] =", 1)[1]
        names = re.findall(r'\{"([A-Z0-9_]+)",', table)
        self.assertEqual([case.name for case in CASES], names)
        self.assertEqual(len(names), 29)

    def test_all_defaults_encode(self):
        for case in CASES:
            value = case.value * 10 if case.kind == "percent" else case.value
            data = command("TEST", 123, case.name, value, case.duration)
            self.assertTrue(data.startswith("TEST 123 " + case.name))
        self.assertEqual(command("CONFIRM", 20, "PASS"), "CONFIRM 20 PASS\n")
        self.assertEqual(command("INFO", 21), "INFO 21\n")

    def test_reject_unsafe_parameters(self):
        for name, value, duration in (("MOTOR_M1_FI", 800, 5001), ("PWR", 0, 10001),
                                      ("BUZZER", 99, 1000), ("LED", 1, 500),
                                      ("UNKNOWN", 0, 500), ("MOTOR_M8_BI", 0, 500)):
            with self.assertRaises(ValueError):
                test_parameters(name, value, duration)

    def test_board_messages(self):
        self.assertEqual(parse("RESULT 123 LED PASS"), ("RESULT", 123, "LED", "PASS"))
        self.assertEqual(parse("BOARD 456 LAMP_UP1 WAIT"), ("BOARD", 456, "LAMP_UP1", "WAIT"))
        self.assertEqual(parse("CAPS BOARD_TEST 2"), ("CAPS", 2))
        self.assertEqual(parse("KEY 456 1 5"), ("KEY", 456, True, 5))
        for text in ("RESULT 123 ADC6 INVALID", "RESULT 65536 LED PASS", "BOARD 1 LED INVALID", "KEY 1 2 5"):
            self.assertEqual(parse(text)[0], "BAD")

    def test_results_correlate_with_latest_request(self):
        class Tree:
            value = None
            def set(self, name, column, value):
                self.value = value
        panel = BoardPanel.__new__(BoardPanel)
        panel.tree = Tree()
        panel.sequences = {1: "LED", 2: "LED"}
        panel.latest = {"LED": 2}
        panel.receive(("RESULT", 1, "LED", "CANCELLED"))
        self.assertIsNone(panel.tree.value)
        panel.receive(("ACK", 2, "TEST"))
        self.assertEqual(panel.tree.value, "已接受 / 测试中")
        panel.receive(("RESULT", 2, "LED", "PASS"))
        self.assertEqual(panel.tree.value, "通过")
        panel.receive(("ACK", 2, "TEST"))
        self.assertEqual(panel.tree.value, "通过")

if __name__ == "__main__":
    unittest.main()
