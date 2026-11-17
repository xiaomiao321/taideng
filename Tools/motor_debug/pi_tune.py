#!/usr/bin/env python3
"""阶梯响应与背隙测量工具：通过 USART3 文本协议驱动关节并分析 JTRACE 数据。

依赖：pyserial（pip install pyserial）。仅用于调试固件（APP_MODE=DEBUG）。

用法示例：
    python pi_tune.py --port COM5 step --joint 3 --to 60000
    python pi_tune.py --port COM5 backlash --joint 3 --center 135000 --delta 5000
    python pi_tune.py --port COM5 get --joint 3
    python pi_tune.py --port COM5 set-pi --joint 3 --values 60 25 12 260 380 800 30 10 6 160 250 600 8000 80 0
"""
import argparse
import csv
import sys
import time

import protocol


class Link:
    """最小的同步串口封装；每条命令带自己的序号。"""

    def __init__(self, port, baud):
        import serial  # 延迟导入，未装 pyserial 时仍可用 sim 子命令

        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.seq = 0

    def next_seq(self):
        self.seq = (self.seq + 1) & 0xFFFF
        return self.seq

    def send(self, kind, **kwargs):
        seq = self.next_seq()
        line = protocol.command(kind, seq, **kwargs)
        self.ser.write(line.encode("ascii"))
        self.ser.flush()
        return seq

    def read(self, timeout=0.2):
        """返回解析后的事件；超时返回 None。"""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            raw = self.ser.readline()
            if not raw:
                continue
            text = raw.decode("ascii", "replace").strip()
            if text:
                return protocol.parse(text)
        return None

    def wait_result(self, seq, timeout):
        """等到指定序号的 JRESULT，期间收集 JT 样本。"""
        samples = []
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            event = self.read(0.2)
            if event is None:
                continue
            if event[0] == "JT":
                samples.append(event)
            elif event[0] == "JRESULT" and event[1] == seq:
                return event[3], samples
        return "TIMEOUT", samples

    def close(self):
        self.ser.close()


def analyze(samples, target, tolerance=2000):
    """从 JT 样本算峰值过冲、稳定时间、稳态误差和往返次数。"""
    if not samples:
        return {}
    angles = [s[5] for s in samples]
    ticks = [s[1] for s in samples]
    start = angles[0]
    direction = 1 if target >= start else -1
    peak = max(angles) if direction > 0 else min(angles)
    overshoot = (peak - target) * direction
    settle = None
    for i, angle in enumerate(angles):
        if abs(target - angle) > tolerance:
            continue
        if all(abs(target - a) <= tolerance for a in angles[i:]):
            settle = ticks[i] - ticks[0]
            break
    reversals = 0
    last = 0
    for angle in angles:
        err = target - angle
        sign = 1 if err > tolerance else (-1 if err < -tolerance else 0)
        if sign and last and sign != last:
            reversals += 1
        if sign:
            last = sign
    duties = [abs(s[7]) for s in samples]
    return {
        "start": start,
        "final": angles[-1],
        "peak": peak,
        "overshoot": overshoot,
        "settle_ms": settle,
        "steady_error": target - angles[-1],
        "reversals": reversals,
        "max_duty": max(duties),
        "saturated": sum(1 for d in duties if d >= 950),
        "samples": len(samples),
    }


def report(metrics):
    if not metrics:
        print("没有收到 JT 样本：确认 JTRACE 已打开、调试固件在跑")
        return
    print("起点 %(start)d  终点 %(final)d  峰值 %(peak)d" % metrics)
    print("过冲 %(overshoot)d mdeg  稳态误差 %(steady_error)d mdeg" % metrics)
    print(
        "稳定时间 %(settle_ms)s ms  来回 %(reversals)d 次  峰值占空比 %(max_duty)d"
        % metrics
    )
    print("饱和样本 %(saturated)d/%(samples)d" % metrics)


def expect_ack(link, seq, timeout=1.0):
    end = time.monotonic() + timeout
    event = None
    while time.monotonic() < end:
        event = link.read(0.2)
        if event and event[0] in ("ACK", "ERR") and event[1] == seq:
            return event
    return event


def cmd_step(link, args):
    link.send("JTRACE", motor=args.joint, duty=1)
    time.sleep(0.1)
    seq = link.send("JMOVE", motor=args.joint, duty=args.to, duration=args.timeout)
    result, samples = link.wait_result(seq, args.timeout / 1000.0 + 1.0)
    print("JRESULT: %s" % result)
    metrics = analyze(samples, args.to, args.tolerance)
    report(metrics)
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["tick", "raw", "spread", "angle", "target", "duty", "phase"])
            for s in samples:
                writer.writerow([s[1], s[3], s[4], s[5], s[6], s[7], s[9]])
        print("已写入 %s" % args.csv)


def read_angle(link, joint, timeout=1.0):
    link.send("JGET", motor=joint)
    angle = None
    end = time.monotonic() + timeout
    while time.monotonic() < end and angle is None:
        event = link.read(0.2)
        if event and event[0] == "JSTATE" and event[2] == joint:
            angle = event[6]  # 下标 6 是实测角度，7 是目标
    return angle


def cmd_backlash(link, args):
    """从两侧分别逼近同一点，两次落点之差即回程误差（含重复性）。"""
    results = {}
    for name, start in (("正向", args.center - args.delta), ("负向", args.center + args.delta)):
        seq = link.send("JMOVE", motor=args.joint, duty=start, duration=8000)
        link.wait_result(seq, 9.0)
        time.sleep(0.3)
        seq = link.send("JMOVE", motor=args.joint, duty=args.center, duration=8000)
        link.wait_result(seq, 9.0)
        time.sleep(0.3)
        results[name] = read_angle(link, args.joint)
        print("%s 落点: %s" % (name, results[name]))
    if results["正向"] is not None and results["负向"] is not None:
        diff = abs(results["正向"] - results["负向"])
        print("回程误差约 %d mdeg（%.2f°），可据此设置 JBL" % (diff, diff / 1000.0))


def cmd_get(link, args):
    link.send("JGET", motor=args.joint)
    end = time.monotonic() + 1.0
    while time.monotonic() < end:
        event = link.read(0.2)
        if not event:
            continue
        if event[0] == "JPI":
            print("JPI", event[3])
            return
        if event[0] == "JTUNE":
            print("JTUNE mode=%d kp=%d low=%d brake=%d" % event[3:7])


def cmd_set_pi(link, args):
    values = [int(v) for v in args.values]
    if len(values) != 15:
        raise SystemExit("需要 15 个值：kp_fast0 kp_slow0 ki0 imax0 dmin0 dmax0 "
                         "kp_fast1 kp_slow1 ki1 imax1 dmin1 dmax1 slowband run_pct flags")
    seq = link.send("JPI", motor=args.joint, duty=values)
    print(expect_ack(link, seq))


def simulate():
    """无硬件自检：用合成样本验证指标计算链路，不代表真实控制性能。"""
    samples = []
    angle = 135000.0
    target = 165000
    for k in range(600):
        tick = (k + 1) * 10
        if tick <= 300:
            angle = 135000.0
        elif tick <= 1500:
            angle += 250.0                   # 上升段
        elif tick <= 1700:
            angle += 3.0                     # 小过冲
        elif tick <= 2400:
            angle -= 4.0                     # 回调
        elif angle < 164900:
            angle += 2.0                     # 缓慢补齐
        duty = 0 if tick > 2400 else 620
        samples.append(("JT", tick, 1, 0, 0, int(angle), target, duty, 0,
                        "MOVE", target))
    report(analyze(samples, target))


def main(argv=None):
    parser = argparse.ArgumentParser(description="关节 PI 阶跃/背隙测量")
    parser.add_argument("--port", help="串口，例如 COM5")
    parser.add_argument("--baud", type=int, default=460800,
                        help="与固件 DEBUG_UART_BAUD 一致，旧固件用 115200")
    parser.add_argument("--joint", type=int, default=1, choices=range(1, 7))
    sub = parser.add_subparsers(dest="action", required=True)

    step = sub.add_parser("step", help="阶跃响应")
    step.add_argument("--to", type=int, required=True, help="目标角度，毫度")
    step.add_argument("--timeout", type=int, default=15000)
    step.add_argument("--tolerance", type=int, default=2000)
    step.add_argument("--csv")
    step.set_defaults(func=cmd_step)

    back = sub.add_parser("backlash", help="回程误差")
    back.add_argument("--center", type=int, required=True)
    back.add_argument("--delta", type=int, default=5000)
    back.set_defaults(func=cmd_backlash)

    get = sub.add_parser("get", help="读取 JPI")
    get.set_defaults(func=cmd_get)

    setp = sub.add_parser("set-pi", help="写入 JPI")
    setp.add_argument("--values", nargs=15, required=True)
    setp.set_defaults(func=cmd_set_pi)

    sub.add_parser("sim", help="无硬件自检")

    args = parser.parse_args(argv)
    if args.action == "sim":
        simulate()
        return 0
    if not args.port:
        parser.error("需要 --port（或使用 sim 子命令）")
    link = Link(args.port, args.baud)
    try:
        args.func(link, args)
    finally:
        link.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
