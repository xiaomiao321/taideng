"""Two-point calibration. Angles use millidegrees on the wire."""
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

@dataclass(frozen=True)
class Calibration:
    raw_a: int
    angle_a: int
    raw_b: int
    angle_b: int
    direction: int
    duty: int = 800
    tolerance: int = 2000

    def validate(self):
        if any(type(v) is not int for v in self.wire()):
            raise ValueError("标定参数必须为整数")
        if not 1 <= self.raw_a <= 4094 or not 1 <= self.raw_b <= 4094 or abs(self.raw_b-self.raw_a) < 100:
            raise ValueError("A/B 读数需为 1～4094，且至少相差 100 个 ADC 计数")
        if not -360000 <= self.angle_a < self.angle_b <= 360000 or self.angle_b-self.angle_a < 1000:
            raise ValueError("B 点角度必须大于 A 点，且至少相差 1°")
        if self.direction not in (-1, 1) or not 500 <= self.duty <= 1000:
            raise ValueError("请选择电机正转方向，并设置 50～100% 占空比")
        if not 100 <= self.tolerance <= 10000 or self.angle_b-self.angle_a <= 2*self.tolerance:
            raise ValueError("到位容差需为 0.1～10°，且小于标定角度跨度的一半")
        return self

    def wire(self):
        return (self.raw_a, self.angle_a, self.raw_b, self.angle_b, self.direction, self.duty, self.tolerance)

    def angle(self, raw):
        self.validate()
        return self.angle_a + (raw-self.raw_a)*(self.angle_b-self.angle_a)/(self.raw_b-self.raw_a)

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
