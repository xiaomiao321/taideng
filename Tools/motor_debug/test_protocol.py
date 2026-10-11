import unittest
from protocol import command, parse, PotModel


class ProtocolTests(unittest.TestCase):
    def test_commands(self):
        self.assertEqual(command("RUN", 12, 8, -800, 500), "RUN 12 M8 -800 500\n")
        self.assertEqual(command("STOP", 13), "STOP 13 ALL\n")
        for args in (("RUN", 1, 0, 800, 500), ("RUN", 1, 1, 1001, 500),
                     ("RUN", 1, 1, 0, 500), ("RUN", 1, 1, 800, -1)):
            with self.assertRaises(ValueError):
                command(*args)

    def test_parse(self):
        self.assertEqual(parse("POT 123 0 1 2 3 4 4095"), ("POT", 123, (0, 1, 2, 3, 4, 4095)))
        self.assertEqual(parse("STATE 123 1 -800 0 1"), ("STATE", 123, 1, -800, False, True))
        self.assertEqual(parse("ACK 12 QUEUED"), ("ACK", 12, "QUEUED"))
        for line in ("POT 123 0 1 2 3 4 4096", "POT xxx 0 1 2 3 4 5", "STATE 1 9 800 0 0",
                     "ACK 65536 STOP"):
            self.assertEqual(parse(line)[0], "BAD")
        self.assertEqual(parse("[INFO] MOTOR ready")[0], "LOG")

    def test_stale_and_baseline(self):
        model = PotModel()
        self.assertTrue(model.stale(1))
        model.update((1, 2, 3, 4, 5, 6), 1)
        model.update((2, 4, 3, 3, 5, 7), 1.1)
        self.assertEqual(model.deltas(), (1, 2, 0, -1, 0, 1))
        self.assertFalse(model.stale(1.2))
        self.assertTrue(model.stale(1.7))
        model.valid = False
        self.assertTrue(model.stale(1.2))


if __name__ == "__main__":
    unittest.main()
