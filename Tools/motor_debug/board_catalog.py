"""The same 29 items as the firmware bring-up table."""
from dataclasses import dataclass
from motor_limits import validate_duty

@dataclass(frozen=True)
class TestCase:
    name: str
    label: str
    kind: str
    value: int
    duration: int
    hint: str

CASES = [
    TestCase("LOG", "调试日志", "none", 0, 500, "检查接收日志，MCU 根据串口发送结果判定。"),
    TestCase("LED", "四个指示灯轮流点亮", "none", 0, 1000, "STATE / COMM / ACT / ERR 依次点亮；时长为每盏灯的保持时间，结束后确认。"),
    TestCase("KEY", "HOME 按键", "none", 0, 15000, "请按板上的 HOME；检测到真实按下自动通过。HOME 也会锁定停止，松开后点击全部停止解锁。"),
    TestCase("BUZZER", "蜂鸣器", "frequency", 2700, 1000, "按指定频率发声，到时自动关闭；听到声音后确认。"),
    TestCase("PWR", "Linux 电源键脉冲", "none", 0, 200, "发送一次 PB12 电源键脉冲，到时释放；用示波器观察后确认。该操作会触发连接的 Linux 电源键。"),
    TestCase("ADC6", "六路电位器 ADC", "none", 0, 500, "执行一次六路扫描，采样成功自动通过；读数及曲线见电机与电位器页。"),
]
for name in ("UP1", "UP2", "UP3", "UP4", "DOWN1", "DOWN2", "DOWN3"):
    CASES.append(TestCase("LAMP_" + name, "灯板 " + name, "percent", 50, 1000,
                          "按指定占空比点亮，到时关闭；观察亮度或 PWM 后确认。"))
for motor in range(1, 9):
    for direction, label in (("FI", "正转"), ("BI", "反转")):
        CASES.append(TestCase(f"MOTOR_M{motor}_{direction}", f"M{motor} {label}", "percent", 80, 500,
                              "点动结束后自动停止，再确认运动结果；最长点动 5000ms，换向前等待 100ms。"))
BY_NAME = {case.name: case for case in CASES}

def test_parameters(name, value, duration):
    if name not in BY_NAME or not isinstance(value, int) or not isinstance(duration, int):
        raise ValueError("Unknown board test")
    case = BY_NAME[name]
    maximum = 5000 if name.startswith("MOTOR_") else 10000 if name in ("BUZZER", "PWR") else 60000
    if not 1 <= duration <= maximum:
        raise ValueError(f"测试时长需为 1–{maximum}ms")
    if case.kind == "none" and value != 0:
        raise ValueError("该测试不使用幅值参数")
    if case.kind == "frequency" and not 100 <= value <= 20000:
        raise ValueError("蜂鸣器频率需为 100–20000Hz")
    if name.startswith("MOTOR_"):
        validate_duty(int(name[7]), value)
    if case.kind == "percent" and not (1 if name.startswith("MOTOR_") else 0) <= value <= 1000:
        raise ValueError("占空比参数超出范围")
