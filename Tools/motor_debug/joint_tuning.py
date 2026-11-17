"""比例积分式整定用的纯逻辑：参数表单换算、阶跃指标、背隙测量流程；不依赖 GUI 和串口。"""
from decimal import Decimal, InvalidOperation
from joint_model import millidegrees

# 每个方向的六项，顺序与 JPI 行一致；方向 0 为角度增大，1 为角度减小。
PI_COLUMNS = (("kp_fast", "快速段 %/°"), ("kp_slow", "慢速段 %/°"), ("ki", "积分 %/(°·s)"),
              ("i_max", "积分限幅 %"), ("duty_min", "起转 %"), ("duty_max", "最高 %"))
PI_DIRECTIONS = ("角度增大", "角度减小")
PI_KEYS = tuple(f"{name}{d}" for d in (0, 1) for name, _ in PI_COLUMNS) + ("slow", "run")
PI_SLOW_BAND_MAX = 45000  # 与固件 pi_is_valid 一致
DUTY_MIN, DUTY_MAX, KP_MAX = 100, 1000, 1000  # 千分比，与固件 MOTOR_DUTY_MIN、MOTOR_DUTY_LIMIT、JOINT_KP_MAX 一致
STEP_TAIL_S = 1.2     # 定位结束后再采这么久，把 HOLD 期间的回弹和纠偏算进指标
BACKLASH_SETTLE_S = 1.5  # 到位后等 HOLD 结束、机构静止再读落点


def tenths(text, name):
    """百分数文本转千分比整数，最多一位小数。"""
    try:
        value = Decimal(str(text).strip()) * 10
        if not value.is_finite() or value < 0 or value != value.to_integral_value():
            raise ValueError()
        return int(value)
    except (ValueError, InvalidOperation):
        raise ValueError(f"{name}须为非负数，最多一位小数") from None


def pi_texts(values):
    """JPI 的 15 个整数转成表单文本，键为 PI_KEYS。"""
    if len(values) != 15:
        raise ValueError("PI 参数须为 15 项")
    texts = {key: f"{value/10:g}" for key, value in zip(PI_KEYS[:12], values[:12])}
    texts["slow"] = f"{values[12]/1000:g}"
    texts["run"] = str(values[13])
    return texts


def pi_values(texts):
    """表单文本转成 JPI 的 15 个整数，按固件的规则先校验一遍。"""
    values = []
    for d, direction in enumerate(PI_DIRECTIONS):
        row = {name: tenths(texts[f"{name}{d}"], f"{direction}方向的{label.split(' ')[0]}")
               for name, label in PI_COLUMNS}
        if max(row["kp_fast"], row["kp_slow"], row["ki"]) > KP_MAX:
            raise ValueError(f"{direction}方向的增益不能超过 {KP_MAX/10:g}")
        if row["i_max"] > DUTY_MAX:
            raise ValueError(f"{direction}方向的积分限幅不能超过 100%")
        if not DUTY_MIN <= row["duty_min"] <= row["duty_max"] <= DUTY_MAX:
            raise ValueError(f"{direction}方向须满足 10% ≤ 起转 ≤ 最高 ≤ 100%")
        values += [row[name] for name, _ in PI_COLUMNS]
    slow = millidegrees(texts["slow"])
    if not 0 <= slow <= PI_SLOW_BAND_MAX:
        raise ValueError(f"慢速段门限须为 0～{PI_SLOW_BAND_MAX/1000:g}°")
    try: run = int(str(texts["run"]).strip())
    except ValueError: raise ValueError("维持比例须为 10～100 的整数") from None
    if not 10 <= run <= 100:
        raise ValueError("维持比例须为 10～100 的整数")
    return (*values, slow, run, 0)


def pi_summary(values):
    """一行说明 MCU 当前的 PI 参数。"""
    def side(v): return f"快 {v[0]/10:g} 慢 {v[1]/10:g} 积分 {v[2]/10:g} 限幅 {v[3]/10:g}% 起转 {v[4]/10:g}% 最高 {v[5]/10:g}%"
    return (f"增大 {side(values[0:6])}；减小 {side(values[6:12])}；"
            f"慢速段 {values[12]/1000:g}° 维持 {values[13]}%")


def step_metrics(samples, target, tolerance):
    """从一次定位的样本算指标。samples 为 (时刻 s, 角度 mdeg, 占空比千分比) 序列，角度单位 mdeg。"""
    if len(samples) < 2:
        return None
    times = [s[0] for s in samples]
    angles = [s[1] for s in samples]
    start = angles[0]
    direction = 1 if target >= start else -1
    peak = max(angles) if direction > 0 else min(angles)
    settle = None
    for i in range(len(angles) - 1, -1, -1):
        if abs(target - angles[i]) > tolerance:
            break
        settle = times[i] - times[0]
    reversals, last = 0, 0
    for angle in angles:
        error = target - angle
        sign = 1 if error > tolerance else (-1 if error < -tolerance else 0)
        if sign and last and sign != last:
            reversals += 1
        last = sign or last
    gaps = sorted(b - a for a, b in zip(times, times[1:]))
    return {"start": start, "final": angles[-1], "overshoot": max(0, (peak - target) * direction),
            "settle_s": settle, "steady_error": target - angles[-1], "reversals": reversals,
            "max_duty": max(abs(s[2]) for s in samples), "period_ms": round(gaps[len(gaps)//2] * 1000),
            "samples": len(samples)}


def metrics_text(joint, result, m):
    """把指标写成曲线上方的一行。"""
    if not m:
        return f"M{joint} 上次定位 {result}：样本不足"
    settle = "未稳定" if m["settle_s"] is None else f"{m['settle_s']:.2f}s"
    return (f"M{joint} 上次定位 {result}：{m['start']/1000:.1f}→{m['final']/1000:.1f}°  过冲 {m['overshoot']/1000:.2f}°  "
            f"稳态误差 {m['steady_error']/1000:+.2f}°  稳定 {settle}  来回 {m['reversals']} 次  "
            f"峰值占空比 {m['max_duty']/10:g}%  采样 {m['period_ms']}ms")


class StepCapture:
    """收集一次定位从下发到结束后 STEP_TAIL_S 的样本。"""
    def __init__(self, target, tolerance):
        self.target, self.tolerance = target, tolerance
        self.samples = []
        self.result = None
        self.result_at = 0.

    def add(self, now, angle, duty):
        self.samples.append((now, angle, duty))

    def finish(self, result, now):
        if self.result is None:
            self.result, self.result_at = result, now

    def ready(self, now):
        """到达后要等尾段采完；其他结果立即可算。"""
        return self.result is not None and (self.result != "REACHED" or now - self.result_at >= STEP_TAIL_S)

    def metrics(self):
        return step_metrics(self.samples, self.target, self.tolerance)


class BacklashRun:
    """回程误差测量：先后从低侧和高侧逼近同一点，两次落点之差即回程误差（含重复性）。

    调用顺序：first() 取第一个目标并下发；每收到该关节的 JRESULT 调 on_result()；
    周期调 tick()，它在机构静止后给出下一个目标，全部走完后 done 为真。
    """
    def __init__(self, joint, center, delta):
        self.joint = joint
        self.targets = (center - delta, center, center + delta, center)
        self.index = 0
        self.wait_until = None
        self.landings = []
        self.error = None

    def first(self):
        return self.targets[0]

    @property
    def done(self):
        return len(self.landings) == 2

    @property
    def active(self):
        return not self.done and self.error is None

    @property
    def backlash(self):
        return abs(self.landings[0] - self.landings[1]) if self.done else None

    def on_result(self, result, now):
        if not self.active:
            return
        if result == "REACHED":
            self.wait_until = now + BACKLASH_SETTLE_S
        else:
            self.error = result

    def tick(self, now, angle):
        """angle 为该关节的新鲜角度，没有时传 None；返回要下发的下一个目标或 None。"""
        if not self.active or self.wait_until is None or now < self.wait_until or angle is None:
            return None
        self.wait_until = None
        if self.index in (1, 3):
            self.landings.append(angle)
        if self.done:
            return None
        self.index += 1
        return self.targets[self.index]

    def progress(self):
        names = ("退到低侧", "从低侧逼近", "退到高侧", "从高侧逼近")
        return f"M{self.joint} 测量背隙：{names[self.index]}（{self.index+1}/4）"
