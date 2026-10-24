"""Operating PWM ranges; lamp PWM is intentionally independent."""
# 全部电机的占空比硬上限（千分比），与 MCU motor_limits.h 的 MOTOR_DUTY_LIMIT 保持一致。
DUTY_LIMIT = 1000

def duty_limits(motor):
    if type(motor) is not int or not 1 <= motor <= 8:
        raise ValueError("电机编号须为 M1～M8")
    return 100, min(1000 if motor <= 6 else 800, DUTY_LIMIT)

def validate_duty(motor, duty, allow_stop=False):
    low, high = duty_limits(motor)
    if type(duty) is not int:
        raise ValueError("占空比参数须为整数千分比")
    if duty == 0 and allow_stop:
        return
    if not low <= abs(duty) <= high:
        raise ValueError(f"M{motor} {'小电机' if motor <= 6 else '大电机'}运行占空比须为 {low/10:g}～{high/10:g}%；停止请使用 stop")
