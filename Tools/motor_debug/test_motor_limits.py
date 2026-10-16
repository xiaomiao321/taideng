import unittest
from motor_limits import validate_duty
from protocol import command
from board_catalog import test_parameters
from joint_model import Calibration
from shell_commands import parse_shell

class MotorLimitsTests(unittest.TestCase):
    def test_all_motors_and_directions(self):
        for motor in range(1, 9):
            high = 1000 if motor <= 6 else 800
            validate_duty(motor, 0, allow_stop=True)
            for sign in (-1, 1):
                for value in (500, high):
                    self.assertTrue(command("RUN", 1, motor, sign*value, 500))
                for value in (0, 1, 499, high+1):
                    with self.assertRaises(ValueError): command("RUN", 1, motor, sign*value, 500)

    def test_board_tests_and_terminal(self):
        for motor in range(1, 9):
            high = 1000 if motor <= 6 else 800
            for direction in ("FI", "BI"):
                name = f"MOTOR_M{motor}_{direction}"
                test_parameters(name, 500, 500)
                test_parameters(name, high, 500)
                for value in (499, high+1):
                    with self.assertRaises(ValueError): test_parameters(name, value, 500)
        for text in ("motor M7 81 500", "motor M8 -100 500", "motor M1 49.9 500",
                     "test MOTOR_M8_FI 100 500"):
            with self.assertRaises(ValueError): parse_shell(text)
        self.assertEqual(parse_shell("motor M1 100 500").request[2], 1000)
        self.assertEqual(parse_shell("motor M7 -80 500").request[2], -800)

    def test_joint_calibration_and_lamps(self):
        for duty in (500, 800, 1000): Calibration(1000,0,3000,180000,1,duty).validate()
        for duty in (0, 499, 1001):
            with self.assertRaises(ValueError): Calibration(1000,0,3000,180000,1,duty).validate()
        for duty in (0, 100, 500, 1000): test_parameters("LAMP_UP1", duty, 1000)

if __name__ == "__main__": unittest.main()
