import unittest
from joint_demo import DemoJoints
from joint_tuning import (PI_KEYS, pi_texts, pi_values, pi_summary, step_metrics, metrics_text, StepCapture,
                          BacklashRun, STEP_TAIL_S, BACKLASH_SETTLE_S)
from protocol import command, parse

VALID = (400, 150, 80, 200, 300, 700, 600, 250, 120, 260, 380, 800, 8000, 80, 0)


class PiFormTests(unittest.TestCase):
    def test_round_trip(self):
        texts = pi_texts(VALID)
        self.assertEqual(set(texts), set(PI_KEYS))
        self.assertEqual((texts["kp_fast0"], texts["ki1"], texts["duty_max1"], texts["slow"], texts["run"]),
                         ("40", "12", "80", "8", "80"))
        self.assertEqual(pi_values(texts), VALID)
        self.assertEqual(command("JPI", 7, 3, pi_values(texts)),
                         "JPI 7 M3 400 150 80 200 300 700 600 250 120 260 380 800 8000 80 0\n")
        self.assertEqual(parse("JPI 100 M3 " + " ".join(map(str, VALID))), ("JPI", 100, 3, VALID))
        self.assertIn("增大 快 40 慢 15", pi_summary(VALID))
        with self.assertRaises(ValueError): pi_texts(VALID[:14])

    def test_validation_matches_firmware(self):
        for key, bad in (("duty_min0", "9.9"), ("duty_min1", "90"), ("duty_max0", "100.1"), ("kp_fast1", "100.1"),
                         ("ki0", "-1"), ("kp_slow0", "1.25"), ("i_max1", "101"), ("slow", "45.001"), ("slow", "-1"),
                         ("run", "9"), ("run", "101"), ("run", "80.5"), ("kp_fast0", ""), ("ki1", "nan")):
            texts = pi_texts(VALID); texts[key] = bad
            with self.assertRaises(ValueError, msg=f"{key}={bad}"): pi_values(texts)
        texts = pi_texts(VALID); texts["duty_min0"] = "10"; texts["slow"] = "45"; texts["run"] = "10"
        self.assertEqual(pi_values(texts)[4], 100)


class StepTests(unittest.TestCase):
    def samples(self, angles, duty=500, period=.01):
        return [(i * period, a, duty if i < len(angles) - 5 else 0) for i, a in enumerate(angles)]

    def test_clean_step(self):
        angles = [90000 + min(i, 100) * 300 for i in range(160)]  # 30° 斜坡后停在目标
        m = step_metrics(self.samples(angles), 120000, 2000)
        self.assertEqual((m["start"], m["final"], m["overshoot"], m["steady_error"], m["reversals"]),
                         (90000, 120000, 0, 0, 0))
        self.assertAlmostEqual(m["settle_s"], .94, places=2)  # 第一次进入 ±2° 且之后不再出去
        self.assertEqual((m["max_duty"], m["period_ms"]), (500, 10))
        text = metrics_text(2, "REACHED", m)
        self.assertIn("M2 上次定位 REACHED", text); self.assertIn("过冲 0.00°", text)

    def test_overshoot_and_reversal(self):
        angles = ([90000 + i * 500 for i in range(70)] +          # 冲到 124.5°
                  [124500 - i * 300 for i in range(1, 16)] +     # 回到 120°
                  [120000] * 30)
        m = step_metrics(self.samples(angles), 120000, 2000)
        self.assertEqual((m["overshoot"], m["reversals"], m["steady_error"]), (4500, 1, 0))
        down = step_metrics(self.samples([120000 - i * 500 for i in range(70)] + [85500] * 20), 90000, 2000)
        self.assertEqual((down["overshoot"], down["settle_s"]), (4500, None))  # 反方向；停在容差外算未稳定
        self.assertIn("未稳定", metrics_text(1, "TIMEOUT", down))
        self.assertIsNone(step_metrics([(0, 1, 0)], 0, 2000))
        self.assertIn("样本不足", metrics_text(1, "REACHED", None))

    def test_capture_waits_for_tail(self):
        cap = StepCapture(120000, 2000)
        cap.add(0, 90000, 500); cap.add(.1, 120000, 0)
        self.assertFalse(cap.ready(5))
        cap.finish("REACHED", 1); cap.finish("CANCELLED", 2)  # 只认第一个结果
        self.assertFalse(cap.ready(1 + STEP_TAIL_S - .01))
        self.assertTrue(cap.ready(1 + STEP_TAIL_S))
        self.assertEqual(cap.metrics()["final"], 120000)
        fault = StepCapture(120000, 2000); fault.finish("STALL", 3)
        self.assertTrue(fault.ready(3))


class BacklashTests(unittest.TestCase):
    def test_sequence(self):
        run = BacklashRun(3, 135000, 5000)
        self.assertEqual(run.first(), 130000)
        self.assertIsNone(run.tick(0, 130000))            # 还没到位
        run.on_result("REACHED", 10)
        self.assertIsNone(run.tick(10 + BACKLASH_SETTLE_S - .1, 130100))
        self.assertIsNone(run.tick(20, None))             # 没有新鲜角度就继续等
        self.assertEqual(run.tick(20, 130100), 135000)
        self.assertIn("从低侧逼近（2/4）", run.progress())
        run.on_result("REACHED", 30)
        self.assertEqual(run.tick(32, 134200), 140000)    # 低侧落点 134.2°
        run.on_result("REACHED", 40)
        self.assertEqual(run.tick(42, 139900), 135000)
        self.assertFalse(run.done)
        run.on_result("REACHED", 50)
        self.assertIsNone(run.tick(52, 135900))           # 高侧落点 135.9°
        self.assertTrue(run.done and not run.active)
        self.assertEqual(run.backlash, 1700)
        self.assertIsNone(run.tick(60, 0))

    def test_abort(self):
        run = BacklashRun(1, 90000, 5000)
        run.on_result("CANCELLED", 1)
        self.assertEqual(run.error, "CANCELLED")
        self.assertFalse(run.active)
        self.assertIsNone(run.tick(10, 90000))
        self.assertIsNone(run.backlash)


class DemoTests(unittest.TestCase):
    def test_demo_reports_tune_and_holds(self):
        lines = []
        demo = DemoJoints(lines.append)
        vals = [1000., 1500., 2000., 2500., 3000., 3500.]
        self.assertIsNone(demo.request("JGET", 1, 2, 800, 500, vals, 0))
        kinds = {parse(line)[0]: parse(line) for line in lines}
        self.assertEqual(kinds["JTUNE"][3], 3)             # 默认比例积分式
        self.assertEqual(len(kinds["JPI"][3]), 15)
        self.assertEqual(kinds["JDIR"][3:], (0, 0, 0))
        lines.clear()
        self.assertIsNone(demo.request("JPI", 2, 2, VALID, 500, vals, 0))
        self.assertIsNone(demo.request("JTUNE", 3, 2, (1, 20, 300, 50), 500, vals, 0))
        self.assertIsNone(demo.request("JDIR", 4, 2, (1, 1500, 4500), 500, vals, 0))
        last = {parse(line)[0]: parse(line) for line in lines}
        self.assertEqual((last["JPI"][3], last["JTUNE"][3:7], last["JDIR"][3:]), (VALID, (1, 20, 300, 50), (1, 1500, 4500)))
        for line in lines: self.assertNotEqual(parse(line)[0], "BAD", line)

    def test_pi_mode_holds_after_reached(self):
        import joint_demo
        clock = [0.]
        real, joint_demo.time.monotonic = joint_demo.time.monotonic, lambda: clock[0]
        try:
            lines = []
            demo = DemoJoints(lines.append)
            vals = [2000.]*6
            self.assertIsNone(demo.request("CAL", 1, 1, (1000, 0, 3000, 180000, 1, 800, 2000), 500, vals, 0))
            self.assertIsNone(demo.request("JMOVE", 2, 1, 100000, 10000, vals, 0))
            phases = []
            for _ in range(80):
                clock[0] += .1
                lines.clear(); demo.step(vals, 0)
                state = next(parse(l) for l in lines if l.startswith("JSTATE") and " M1 " in l)
                self.assertNotEqual(state[0], "BAD")
                if not phases or phases[-1] != state[9]: phases.append(state[9])
            self.assertEqual(phases[-2:], ["HOLD", "DONE"])
            # 比例式到位后直接 DONE；HOLD 期间取消回到空闲。
            self.assertIsNone(demo.request("JTUNE", 3, 1, (1, 10, 300, 50), 500, vals, 0))
            self.assertIsNone(demo.request("JMOVE", 4, 1, 90000, 10000, vals, 0))
            phases.clear()
            for _ in range(60):
                clock[0] += .1
                demo.step(vals, 0); phases.append(demo.phases[1])
            self.assertNotIn("HOLD", phases); self.assertEqual(phases[-1], "DONE")
            demo.holds[1] = clock[0]; demo.phases[1] = "HOLD"
            demo.cancel()
            self.assertEqual((demo.phases[1], demo.holds), ("IDLE", {}))
        finally:
            joint_demo.time.monotonic = real


if __name__ == "__main__": unittest.main()
