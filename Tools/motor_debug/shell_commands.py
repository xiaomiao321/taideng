"""Human-facing commands translated to the existing serial protocol."""
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import re
import shlex
from board_catalog import BY_NAME
from protocol import command
from joint_model import millidegrees

HELP = """命令（占空比用百分数，时长用 ms）：
运行范围：M1～M6 小电机 10～100%；M7/M8 大电机 10～80%。停止使用 stop。
  motor M1 80 500         正转点动；-80 反转；时长 0 持续运行
  angle M1 45 [10000]     使用已下发标定定位到 45°，默认超时 10 秒
  joint M1               查询关节状态、MCU 标定及允许范围
  jlim M1 10 170         修改允许运动范围为 10～170°（不改标定）
  jnom M1 2048 90 down + 80 2 10 170 [47]
                         理论标定：ADC 2048 处为 90°，角度增大时读数
                         down 减小 / up 增大，正转使角度 + 增大 / - 减小，
                         占空比 80%，容差 2°，允许 10～170°，阻值 47kΩ
                         （省略按 45kΩ）。电位器须 1:1 装在关节轴上
  tune M1 p 1 30 50       比例式控制：每度误差加 1% 占空比，起转 30%，
                         到位和换向时制动 50ms，保存到 MCU；tune M1 bang 改为开关式
  trace M1 on / off       每 10ms 上报 M1 的读数、噪声、角度、占空比，
                         自动存成 records 目录下的 CSV
  pwm [5 brake]          查询或设置电机 PWM：频率 1/2/4/5/10/20kHz，
                         关断段 brake 刹车 / coast 滑行，须先停电机；上电默认 5 brake
  lamp UP1 50 1000        灯板 50%，保持 1000ms 后关闭
  buzzer 2700 1000        蜂鸣器 2700Hz，保持 1000ms
  power [200]            一次 Linux 电源键脉冲，会作用于连接的 Linux
  led [1000]             四灯轮流点亮，每盏保持指定时长
  log                    调试日志测试
  adc / key / status     查看最新电位器 / HOME / 控制状态
  adc test               执行六路 ADC 测试
  key test [15000]       等待板上 HOME，默认超时 15 秒
  test                   列出全部 29 个测试项目
  test 项目 [参数 时长]  执行任一板级项目；省略参数使用默认值
                         占空比参数仍为百分数，如 test MOTOR_M1_BI 80 500
  pass / fail            输出停止后确认当前板级测试的结果
  stop                   关闭全部输出 / HOME 松开后解锁
  info                   查询固件能力
  watch on / off         显示 / 隐藏周期遥测，默认隐藏
  help / history / clear 帮助 / 历史命令 / 清屏
↑ ↓ 历史；Tab 补全；Ctrl+L 清屏；输入框 Ctrl+C 或 ESC 停止。
序号与心跳自动处理。命令一次一行，不执行操作系统命令。"""

COMMANDS = ("help", "motor", "angle", "joint", "jlim", "jnom", "tune", "trace", "pwm", "lamp", "buzzer", "power", "led", "log", "adc", "key",
            "status", "test", "pass", "fail", "stop", "info", "watch", "history", "clear")

@dataclass(frozen=True)
class ShellCommand:
    action: str
    request: tuple | None = None
    argument: str | None = None

def integer(s):
    if not re.fullmatch(r"[0-9]+", s):
        raise ValueError("频率和时长须为非负整数")
    return int(s)

def percent(s):
    try:
        value = Decimal(s)
        if not value.is_finite() or not -100 <= value <= 100 or value * 10 != (value * 10).to_integral_value():
            raise ValueError()
        return int(value * 10)
    except (InvalidOperation, ValueError):
        raise ValueError("占空比范围 −100～100%，最多一位小数") from None

def kilohms(s):
    try:
        value = Decimal(s)
        if not value.is_finite() or value <= 0 or value * 1000 != (value * 1000).to_integral_value():
            raise ValueError()
        return int(value * 1000)
    except (InvalidOperation, ValueError):
        raise ValueError("电位器阻值以 kΩ 为单位，如 45 或 47.5") from None

def sign(s, positive, negative, what):
    if s.lower() in positive: return 1
    if s.lower() in negative: return -1
    raise ValueError(what)

def send(kind, target=1, value=800, duration=500):
    request = (kind, target, value, duration)
    command(kind, 0, target, value, duration)  # reuse wire validation
    return ShellCommand("send", request)

def parse_shell(text):
    if "\n" in text or "\r" in text:
        raise ValueError("请一次输入一行命令")
    p = shlex.split(text, comments=True)
    if not p:
        return ShellCommand("empty")
    name, args = p[0].lower(), p[1:]
    if name in ("help", "clear", "history", "status") and not args:
        return ShellCommand(name)
    if name in ("adc", "key") and not args:
        return ShellCommand(name)
    if name == "test" and not args:
        return ShellCommand("list")
    if name == "watch" and len(args) == 1 and args[0].lower() in ("on", "off"):
        return ShellCommand("watch", argument=args[0].lower())
    if name == "stop" and not args:
        return send("STOP")
    if name == "info" and not args:
        return send("INFO")
    if name in ("pass", "fail") and not args:
        return send("CONFIRM", name.upper())
    if name == "motor" and len(args) == 3 and re.fullmatch(r"[Mm][1-8]", args[0]):
        return send("RUN", int(args[0][1]), percent(args[1]), integer(args[2]))
    if name == "angle" and len(args) in (2, 3) and re.fullmatch(r"[Mm][1-6]", args[0]):
        return send("JMOVE", int(args[0][1]), millidegrees(args[1]), integer(args[2]) if len(args) == 3 else 10000)
    if name == "joint" and len(args) == 1 and re.fullmatch(r"[Mm][1-6]", args[0]):
        return send("JGET", int(args[0][1]))
    if name == "jlim" and len(args) == 3 and re.fullmatch(r"[Mm][1-6]", args[0]):
        return send("JLIM", int(args[0][1]), (millidegrees(args[1]), millidegrees(args[2])))
    if name == "jnom" and len(args) in (9, 10) and re.fullmatch(r"[Mm][1-6]", args[0]):
        if not re.fullmatch(r"[0-9]+", args[1]):
            raise ValueError("参考点 ADC 读数须为整数")
        values = (int(args[1]), millidegrees(args[2]),
                  sign(args[3], ("up",), ("down",), "读数方向须为 up 或 down"),
                  sign(args[4], ("+",), ("-",), "正转方向须为 + 或 -"),
                  percent(args[5]), millidegrees(args[6]), millidegrees(args[7]), millidegrees(args[8]),
                  kilohms(args[9]) if len(args) == 10 else 0)
        return send("JNOM", int(args[0][1]), values)
    if name == "tune" and len(args) == 2 and args[1].lower() == "bang" and re.fullmatch(r"[Mm][1-6]", args[0]):
        return send("JTUNE", int(args[0][1]), (0, 30, 500, 50))
    if name == "tune" and len(args) == 5 and args[1].lower() == "p" and re.fullmatch(r"[Mm][1-6]", args[0]):
        return send("JTUNE", int(args[0][1]), (1, percent(args[2]), percent(args[3]), integer(args[4])))
    if name == "trace" and len(args) == 2 and args[1].lower() in ("on", "off") and re.fullmatch(r"[Mm][1-6]", args[0]):
        return send("JTRACE", int(args[0][1]), int(args[1].lower() == "on"))
    if name == "pwm" and not args:
        return send("MPWM", None)
    if name == "pwm" and len(args) == 2:
        return send("MPWM", integer(args[0]), int(sign(args[1], ("brake",), ("coast",), "衰减方式须为 brake 或 coast") > 0))
    if name == "lamp" and len(args) == 3:
        return send("TEST", "LAMP_" + args[0].upper(), percent(args[1]), integer(args[2]))
    if name == "buzzer" and len(args) == 2:
        return send("TEST", "BUZZER", integer(args[0]), integer(args[1]))
    if name in ("power", "led") and len(args) <= 1:
        return send("TEST", "PWR" if name == "power" else "LED", 0,
                    integer(args[0]) if args else 200 if name == "power" else 1000)
    if name == "log" and not args:
        return send("TEST", "LOG", 0, 500)
    if name == "adc" and args == ["test"]:
        return send("TEST", "ADC6", 0, 500)
    if name == "key" and 1 <= len(args) <= 2 and args[0].lower() == "test":
        return send("TEST", "KEY", 0, integer(args[1]) if len(args) == 2 else 15000)
    if name == "test" and len(args) in (1, 3):
        target = args[0].upper()
        if target not in BY_NAME:
            raise ValueError("未知测试项目，输入 test 查看列表")
        case = BY_NAME[target]
        value = case.value * 10 if case.kind == "percent" else case.value
        duration = case.duration
        if len(args) == 3:
            value = percent(args[1]) if case.kind == "percent" else integer(args[1])
            duration = integer(args[2])
        return send("TEST", target, value, duration)
    raise ValueError("命令或参数不正确，输入 help 查看用法")

def complete(text):
    """Return whole-line candidates, preserving typed arguments."""
    words = text.split()
    trailing = bool(text) and text[-1].isspace()
    if not words:
        return list(COMMANDS)
    if len(words) == 1 and not trailing:
        return [c for c in COMMANDS if c.startswith(words[0].lower())]
    prefix = "" if trailing else words[-1]
    position = len(words) if trailing else len(words) - 1
    options = []
    if position == 1:
        options = {"motor": [f"M{i}" for i in range(1, 9)],
                   "angle": [f"M{i}" for i in range(1, 7)], "joint": [f"M{i}" for i in range(1, 7)],
                   "jlim": [f"M{i}" for i in range(1, 7)], "jnom": [f"M{i}" for i in range(1, 7)],
                   "tune": [f"M{i}" for i in range(1, 7)], "trace": [f"M{i}" for i in range(1, 7)],
                   "lamp": ["UP1", "UP2", "UP3", "UP4", "DOWN1", "DOWN2", "DOWN3"],
                   "test": list(BY_NAME), "watch": ["on", "off"],
                   "adc": ["test"], "key": ["test"]}.get(words[0].lower(), [])
    base = text if trailing else text[:len(text) - len(prefix)]
    return [base + option for option in options if option.lower().startswith(prefix.lower())]
