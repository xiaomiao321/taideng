# PotAngle —— 电位器角度采样库 (STM32 HAL)

面向"电位器角度旋钮"的 ADC 采样库: 一次调用完成
**连采平均 → (可选一阶 IIR) → 整数角度换算**;
默认按**最低延迟**配置 (IIR 关闭 + 1kHz 调用, 总延迟约 1~2ms),
需要抑噪时一键开启 IIR;
全程整数运算 (无浮点), 不使用中断/定时器。

> 当前工程: 45kΩ / 270° 电位器, `3V3-1k-UP-[电位器]-DOWN-1k-GND`,
> MID 串 1k 接 **PA4 (ADC1_IN4)**, ADC 脚对地 100nF (C11/C12)。

## 滤波链

```
ADC 连采 N 次 ─平均─> avg ─一阶IIR─> filtered ─线性映射─> angle_deg (整数°, 0~270)
 N = 8 (默认)              shift = 0 (默认, = 关闭, 零额外延迟)
 噪声 ÷2.8                 开启后 τ ≈ 2^shift / 采样率
```

- **帧内平均**: N 次连续采样求平均, 随机噪声降 √N 倍 (N=8 → ÷2.8, 忙等仅 ~0.06ms,
  对延迟的贡献可忽略);
- **一阶 IIR** (可选): `filtered += (avg - filtered + 2^(shift-1)) >> shift`,
  四舍五入避免小差值时整数截断卡死; **默认 shift=0 = 关闭** (零额外延迟),
  开启后 τ ≈ 2^shift / 采样率 (shift=1 @1kHz → 2ms);
- **整数角度**: 四舍五入 + 端点限幅, 不出现负值或超量程。

## 硬件与 CubeMX 前提

| 项目 | 要求 |
|---|---|
| ADC | 12bit 右对齐 / 单通道 / 非扫描 / 软件触发单次转换 |
| 采样时间 | ≥ **71.5 周期** (ADCCLK=12MHz 约 6µs); 电位器源阻抗高 (中心 ≈12.75kΩ), 太短读数偏低 |
| 引脚 | PA4 (ADC1_IN4), 分压电路见上 |
| 校准 | `PotAngle_Init()` 内部自动做 ADC 上电校准, 无需再手动调用 |
| 节奏 | 推荐 1kHz (1ms) 调用 `PotAngle_Update()`; 单帧忙等约 0.06ms |

## 快速上手

```c
#include "pot_angle.h"

PotAngle_Handle pot1; /* 全局句柄, Ozone 可直接 Watch 其字段 */

/* 初始化: 绑定 ADC + 上电校准 + 先采一帧 (上电即稳定) */
if (PotAngle_Init(&pot1, &hadc1) == false)
{
    Error_Handler();
}

while (1)
{
    PotAngle_Update(&pot1); /* 推荐 1ms 一次 (1kHz): 总延迟约 1~2ms */
    HAL_Delay(1);
}
```

## API

| 函数 | 说明 |
|---|---|
| `PotAngle_Init(pot, hadc)` | 绑定 ADC、上电校准、先采一帧; false=失败 |
| `PotAngle_Update(pot)` | 采一帧并更新滤波/角度 (帧内忙等 ~0.06ms) |
| `PotAngle_SetEndpoints(pot, raw0, raw270)` | 运行期改端点标定值 |
| `PotAngle_SetFilter(pot, burst, shift)` | 运行期改连采次数 (1~32) / IIR 系数 (0~8, 0=关闭滤波) |
| `PotAngle_GetRaw / GetAvg / GetFiltered / GetAngleDeg` | 读各阶段结果 |

观测字段 (句柄公开, 已加 volatile): `raw` / `avg` / `filtered` / `angle_deg`。
Ozone Watch 写 `pot1.filtered`、`pot1.angle_deg` 即可。

## 参数与调优

| 宏 (pot_angle.h) | 默认 | 调法 |
|---|---|---|
| `POT_ANGLE_BURST_SAMPLES` | 8 | 加大更稳 (忙等线性增加); 范围 1~32; 要更快用 1~4 |
| `POT_ANGLE_EMA_SHIFT` | 0 | **默认 0 = 关闭 (最低延迟)**; 读数抖时开 1~3 (τ ≈ 2^shift / 采样率, 成倍增加) |
| `POT_ANGLE_RAW_AT_0DEG` | 87 | 实测 0° 端点后校准 |
| `POT_ANGLE_RAW_AT_270DEG` | 4008 | 实测满量程端点后校准 |
| `POT_ANGLE_FULL_DEG` | 270 | 机械满量程角度 |

**延迟/噪声对照** (以单次采样 ±10 计数抖动为参照):

| 配置 | 调用频率 | 等效总延迟 | 滤波后抖动 |
|---|---|---|---|
| **现默认 8 / shift 0** | 1kHz | **~1~2ms** | ±3.5 (~0.24°) |
| 轻度平滑 8 / shift 1 | 1kHz | ~2~3ms | ±2.5 (~0.17°) |
| 旧默认 16 / shift 3 | 100Hz | ~250ms | ±1 |

## 角度标定步骤

1. 电位器旋到机械下端, 读 `pot1.filtered`, 写入 `POT_ANGLE_RAW_AT_0DEG`
   (不改头文件也可: `PotAngle_SetEndpoints()` 运行时设置);
2. 旋到机械上端, 读 `pot1.filtered`, 写入 `POT_ANGLE_RAW_AT_270DEG`;
3. 必须满足 `raw_at_0deg < raw_at_270deg`; 方向反了就把电位器两端线对调。

## 注意

- `PotAngle_Update()` 是短时**忙等** (8 次转换约 0.06ms), 其余时间不占 CPU;
- **延迟构成**: 默认配置下只剩"调用间隔" (≤1ms) + 帧内连采 (~0.06ms); 开启
  IIR 才叠加 τ ≈ 2^shift / 采样率; 硬件侧 100nF 的 RC 约 1ms 量级 (中心位置),
  保持现状即可 —— **但不要并 µF 级电容** (1µF 就约 14ms, 软件补不回来);
- 多个通道共用一个 ADC 时顺序调用即可 (每次采样前都重新 Start/Stop).
