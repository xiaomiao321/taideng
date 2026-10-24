"""Joint calibration: measured two-point or nominal (theory + one reference).

Angles use millidegrees on the wire. The allowed motion range (limits) is kept
separate from the calibration points; the MCU reports it in a JLIM line.
"""
from dataclasses import dataclass, asdict
from decimal import Decimal, InvalidOperation
import json

def millidegrees(text):
    try:
        value = Decimal(str(text))
        if not value.is_finite() or not -360 <= value <= 360 or value * 1000 != (value * 1000).to_integral_value():
            raise ValueError()
        return int(value * 1000)
    except (ValueError, InvalidOperation):
        raise ValueError("角度需为 −360～360°，最多三位小数") from None

ADC_FULL = 4095
POT_TRAVEL_MDEG = 270000      # 电位器标称电气行程
POT_DEFAULT_OHMS = 45000      # 阻值填 0 时 MCU 采用的标称值
POT_END_OHMS = 1000           # 3V3-1k-[电位器]-1k-GND
POT_MIN_OHMS, POT_MAX_OHMS = 1000, 1000000


def validate_limits(limit_min, limit_max, tolerance):
    if type(limit_min) is not int or type(limit_max) is not int:
        raise ValueError("允许范围必须为整数毫度")
    if not -360000 <= limit_min < limit_max <= 360000 or limit_max-limit_min <= 2*tolerance:
        raise ValueError("允许范围须在 −360～360° 内，最小值小于最大值，且宽于两倍容差")


def _divide_round(numerator, denominator):
    """Same rounding as the firmware: half away from zero, C truncation."""
    half = denominator // 2
    value = abs(numerator) + half
    quotient = value // denominator
    return quotient if numerator >= 0 else -quotient


@dataclass(frozen=True)
class Calibration:
    raw_a: int
    angle_a: int
    raw_b: int
    angle_b: int
    direction: int
    duty: int = 800
    tolerance: int = 2000
    limit_min: int | None = None   # None：未知，按标定点 A～B
    limit_max: int | None = None

    @property
    def limits(self):
        if self.limit_min is None or self.limit_max is None:
            return self.angle_a, self.angle_b
        return self.limit_min, self.limit_max

    def with_limits(self, limit_min, limit_max):
        return Calibration(*self.wire(), limit_min, limit_max)

    def validate(self):
        if any(type(v) is not int for v in self.wire()):
            raise ValueError("标定参数必须为整数")
        if not 1 <= self.raw_a <= 4094 or not 1 <= self.raw_b <= 4094 or abs(self.raw_b-self.raw_a) < 100:
            raise ValueError("A/B 读数需为 1～4094，且至少相差 100 个 ADC 计数")
        if not -360000 <= self.angle_a < self.angle_b <= 360000 or self.angle_b-self.angle_a < 1000:
            raise ValueError("B 点角度必须大于 A 点，且至少相差 1°")
        if self.direction not in (-1, 1) or not 100 <= self.duty <= 1000:
            raise ValueError("请选择电机正转方向，并设置 10～100% 占空比")
        if not 100 <= self.tolerance <= 10000:
            raise ValueError("到位容差需为 0.1～10°")
        validate_limits(*self.limits, self.tolerance)
        low, high = sorted((self.firmware_angle(1), self.firmware_angle(ADC_FULL-1)))
        if not (low <= self.limits[0] and self.limits[1] <= high):
            raise ValueError(f"允许范围须在电位器可测区间 {low/1000:g}～{high/1000:g}° 内")
        return self

    def firmware_angle(self, raw):
        """Angle exactly as the MCU computes it (integer, C truncation)."""
        numerator = (raw-self.raw_a)*(self.angle_b-self.angle_a)
        denominator = self.raw_b-self.raw_a
        quotient = abs(numerator)//abs(denominator)
        return self.angle_a + (quotient if (numerator >= 0) == (denominator > 0) else -quotient)

    def wire(self):
        return (self.raw_a, self.angle_a, self.raw_b, self.angle_b, self.direction, self.duty, self.tolerance)

    def angle(self, raw):
        self.validate()
        return self.angle_a + (raw-self.raw_a)*(self.angle_b-self.angle_a)/(self.raw_b-self.raw_a)

@dataclass(frozen=True)
class Nominal:
    """Theory calibration: 270° pot mounted 1:1 on the joint shaft."""
    raw_ref: int
    angle_ref: int
    sensor_direction: int          # 角度增大时 ADC 增大为 1，减小为 -1
    direction: int                 # 电机正转使角度增大为 1，减小为 -1
    duty: int = 800
    tolerance: int = 2000
    limit_min: int = 0
    limit_max: int = 270000
    pot_ohms: int = 0              # 0：采用标称 45kΩ

    def wire(self):
        return (self.raw_ref, self.angle_ref, self.sensor_direction, self.direction, self.duty,
                self.tolerance, self.limit_min, self.limit_max, self.pot_ohms)

    def validate(self):
        if any(type(v) is not int for v in self.wire()):
            raise ValueError("理论标定参数必须为整数")
        if not 1 <= self.raw_ref <= ADC_FULL-1:
            raise ValueError("参考点 ADC 读数需为 1～4094")
        if not -360000 <= self.angle_ref <= 360000:
            raise ValueError("参考角度需为 −360～360°")
        if self.sensor_direction not in (-1, 1) or self.direction not in (-1, 1):
            raise ValueError("请选择电位器读数方向和电机正转方向")
        if not 100 <= self.duty <= 1000 or not 100 <= self.tolerance <= 10000:
            raise ValueError("占空比需为 10～100%，容差需为 0.1～10°")
        validate_limits(self.limit_min, self.limit_max, self.tolerance)
        if self.pot_ohms and not POT_MIN_OHMS <= self.pot_ohms <= POT_MAX_OHMS:
            raise ValueError("电位器阻值需为 1kΩ～1MΩ，或留空采用 45kΩ")
        return self

    def endpoints(self):
        """ADC readings at the two electrical ends of the pot."""
        ohms = self.pot_ohms or POT_DEFAULT_OHMS
        total = ohms + 2*POT_END_OHMS
        return (_divide_round(ADC_FULL*POT_END_OHMS, total),
                _divide_round(ADC_FULL*(ohms+POT_END_OHMS), total))

    def calibration(self):
        """The two-point calibration the MCU will derive (same integer math)."""
        self.validate()
        ohms = self.pot_ohms or POT_DEFAULT_OHMS
        total = ohms + 2*POT_END_OHMS
        low, high = self.endpoints()
        raws = [self.raw_ref + _divide_round(self.sensor_direction*(angle-self.angle_ref)*ADC_FULL*ohms,
                                             total*POT_TRAVEL_MDEG)
                for angle in (self.limit_min, self.limit_max)]
        if not low <= self.raw_ref <= high:
            raise ValueError(f"参考点读数须在电位器两端读数 {low}～{high} 之间")
        if not all(low <= raw <= high for raw in raws):
            raise ValueError("允许范围超出电位器 270° 电气行程，请检查参考角度、读数方向或范围")
        return Calibration(raws[0], self.limit_min, raws[1], self.limit_max, self.direction,
                           self.duty, self.tolerance, self.limit_min, self.limit_max).validate()


def encode_profile(configs):
    for key, cal in configs.items():
        if str(key) not in tuple(map(str, range(1, 7))):
            raise ValueError("只有 M1～M6 支持关节标定")
        cal.validate()
    return json.dumps({"version": 1, "joints": {str(k): asdict(v) for k, v in configs.items()}},
                      ensure_ascii=False, indent=2)

def decode_profile(text):
    try:
        data = json.loads(text)
        if type(data) is not dict or type(data.get("version")) is not int or data.get("version") != 1 or type(data.get("joints")) is not dict:
            raise ValueError("文件格式或版本不正确")
        result = {}
        for key, value in data["joints"].items():
            if key not in tuple(map(str, range(1, 7))) or type(value) is not dict:
                raise ValueError("只允许 M1～M6 标定参数")
            result[int(key)] = Calibration(**value).validate()
        return result
    except (TypeError, KeyError, json.JSONDecodeError) as exc:
        raise ValueError("标定文件内容不正确") from exc
