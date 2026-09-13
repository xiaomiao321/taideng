/**
  ******************************************************************************
  * @file    pot_angle.h
  * @brief   电位器角度采样库 (45kΩ/270° 电位器 + ADC1_IN4 / PA4)
  *
  * 一次调用完成整条采样链 (默认最低延迟配置: 1kHz 下总延迟约 1~2ms):
  *   ADC 连采 N 次 ─平均─> avg ─一阶IIR─> filtered ─线性映射─> angle_deg
  *   N = POT_ANGLE_BURST_SAMPLES (默认 8): 随机噪声约降 √N 倍 (÷2.8);
  *   IIR 系数 2^POT_ANGLE_EMA_SHIFT (默认 0 = 关闭): 纯靠帧内平均抑噪,
  *   需要更稳时再开启 1~8 (每加 1 时间常数翻倍), 详见 README.
  *
  * 使用前提 (CubeMX):
  *   1. ADC1: 12bit 右对齐 / 单通道 / 非扫描 / 软件触发单次转换;
  *   2. 采样时间 >= 71.5 周期 (ADCCLK=12MHz 约 6µs) —— 电位器源阻抗高
  *      (中心位置等效约 12.75kΩ), 采样时间太短读数会明显偏低;
  *   3. 分压电路: 3V3-1k-UP-[电位器 45kΩ]-DOWN-1k-GND, MID 串 1k 接 ADC 脚,
  *      ADC 脚对地 100nF (µF 级电容会明显增加延迟, 低延迟场景不要加);
  *   4. 建议 1kHz 调 PotAngle_Update() (间隔 1ms): 单帧忙等约 0.06ms
  *      (8 次 x ~7.5µs), 其余时间不占用 CPU, 不使用中断/定时器.
  ******************************************************************************
  */

#ifndef POT_ANGLE_H
#define POT_ANGLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "stm32f1xx_hal.h" /* 更换芯片系列时改为对应的 stm32xxxx_hal.h */

/* ---------------- 采样与滤波参数 (编译期默认值, 可在运行时修改) ---------------- */

/** @brief 每帧连采平均次数 (1 ~ 32), 默认 8
  * @note  随机噪声约降为单次的 1/√N; 8 次单帧忙等约 0.06ms @12MHz ADCCLK.
  *        对延迟敏感可先用 1~4; 调大更稳但帧内时间变长. */
#ifndef POT_ANGLE_BURST_SAMPLES
#define POT_ANGLE_BURST_SAMPLES 8U
#endif

/** @brief 一阶 IIR 平滑系数 (0 ~ 8), 默认 0 = 关闭滤波 (最低延迟)
  * @note  关闭时只靠帧内平均抑噪, filtered 即等于 avg, 零额外延迟;
  *        开启后时间常数 ≈ 2^shift / 采样率 (shift=1 @1kHz -> 2ms),
  *        调大更平滑但延迟成倍增加. */
#ifndef POT_ANGLE_EMA_SHIFT
#define POT_ANGLE_EMA_SHIFT 0U
#endif

/** @brief 0° 端点对应的原始 ADC 值 (理论值, 首次实测后校准) */
#ifndef POT_ANGLE_RAW_AT_0DEG
#define POT_ANGLE_RAW_AT_0DEG 87U
#endif

/** @brief 满量程端点对应的原始 ADC 值 (理论值, 首次实测后校准)
  * @note  必须大于 POT_ANGLE_RAW_AT_0DEG; 方向反了就把电位器两端线对调. */
#ifndef POT_ANGLE_RAW_AT_270DEG
#define POT_ANGLE_RAW_AT_270DEG 4008U
#endif

/** @brief 机械满量程角度 (°): 45kΩ 270° 电位器 = 270 */
#ifndef POT_ANGLE_FULL_DEG
#define POT_ANGLE_FULL_DEG 270U
#endif

/** @brief 电位器实例句柄: 一个句柄对应一个电位器 + 一个 ADC 通道 */
typedef struct
{
    ADC_HandleTypeDef *hadc; /**< 绑定的 ADC 句柄 (由调用者拥有, 如 &hadc1) */

    uint16_t raw_at_0deg;   /**< 0° 端点标定 raw (默认 POT_ANGLE_RAW_AT_0DEG) */
    uint16_t raw_at_270deg; /**< 满量程端点标定 raw (默认 POT_ANGLE_RAW_AT_270DEG) */

    uint8_t burst_samples; /**< 每帧连采平均次数 (默认 8) */
    uint8_t ema_shift;     /**< IIR 系数 2^ema_shift (默认 0 = 关闭滤波) */

    /* ---- 观测量: 供调试器/上位机读取 (Ozone Watch 这些字段即可) ---- */
    volatile uint16_t raw;       /**< 本帧最后一次原始采样 (抖动最大, 对比用) */
    volatile uint16_t avg;       /**< 本帧 N 次平均 */
    volatile uint16_t filtered;  /**< 一阶 IIR 输出 (shift=0 时等于 avg); 角度由它换算 */
    volatile uint16_t angle_deg; /**< 最终整数角度 0 ~ 270 (四舍五入, 端点限幅) */
} PotAngle_Handle;

/* -------------------------------------------------------------------------- */
/* 接口                                                                        */
/* -------------------------------------------------------------------------- */

/**
  * @brief  初始化采样库: 绑定 ADC + 上电校准 + 先采一帧
  * @param  pot  电位器句柄
  * @param  hadc 已初始化的 ADC 句柄 (如 &hadc1)
  * @retval true  成功
  * @retval false 参数非法或 ADC 校准失败
  * @note   内部完成 HAL_ADCEx_Calibration_Start (F1 必须在任何一次转换前
  *         校准一次, 否则读数不准), 调用者无需再手动校准;
  *         结束时先采一帧, 让 IIR 从真实值起步, 上电即稳定, 不用等收敛.
  */
bool PotAngle_Init(PotAngle_Handle *pot, ADC_HandleTypeDef *hadc);

/**
  * @brief  运行期修改角度端点标定值 (单位: 原始 ADC 值)
  * @param  pot          电位器句柄
  * @param  raw_at_0deg  机械 0° 位置的 raw
  * @param  raw_at_270deg 机械满量程位置的 raw
  * @note   两个参数必须满足 raw_at_0deg < raw_at_270deg (等价于角度随 raw
  *         增大而增大); 否则角度恒为 0. 例: PotAngle_SetEndpoints(&pot1, 90, 4010);
  */
void PotAngle_SetEndpoints(PotAngle_Handle *pot, uint16_t raw_at_0deg, uint16_t raw_at_270deg);

/**
  * @brief  运行期修改采样/滤波参数
  * @param  pot           电位器句柄
  * @param  burst_samples 每帧连采平均次数 (1 ~ 32)
  * @param  ema_shift     IIR 系数 (0 ~ 8): 0 = 关闭滤波 (最低延迟),
  *                       时间常数 ≈ 2^ema_shift / 采样率
  * @retval true  成功
  * @retval false 参数非法 (越界) 或句柄为空
  * @note   立即生效; 例: PotAngle_SetFilter(&pot1, 8, 1) 轻度平滑;
  *         PotAngle_SetFilter(&pot1, 16, 3) 很稳但慢很多.
  */
bool PotAngle_SetFilter(PotAngle_Handle *pot, uint8_t burst_samples, uint8_t ema_shift);

/**
  * @brief  采样一帧: N 次连采平均 -> 一阶 IIR -> 整数角度  [帧内忙等 ~0.06ms]
  * @param  pot 电位器句柄 (需已 PotAngle_Init)
  * @note   调用频率决定刷新率与 IIR 时间常数: 低延迟推荐 1kHz (1ms 一次);
  *         2~4ms 间隔也完全可用 (延迟与 IIR 时间常数随之变大).
  *         内部为短时忙等 (每次转换 ~7µs), 无 HAL_Delay; 转换失败 (超时)
  *         的样本自动丢弃, 整帧全失败则保留上一帧输出;
  *         结果见 pot->raw / avg / filtered / angle_deg.
  */
void PotAngle_Update(PotAngle_Handle *pot);

/**
  * @brief  读取本帧最后一次原始采样值 (抖动最大, 用于对比滤波效果)
  */
uint16_t PotAngle_GetRaw(const PotAngle_Handle *pot);

/**
  * @brief  读取本帧 N 次平均值
  */
uint16_t PotAngle_GetAvg(const PotAngle_Handle *pot);

/**
  * @brief  读取一阶 IIR 输出值 (最平滑, 角度由它换算)
  */
uint16_t PotAngle_GetFiltered(const PotAngle_Handle *pot);

/**
  * @brief  读取最终角度 (整数° 0 ~ 270, 四舍五入 + 端点限幅)
  */
uint16_t PotAngle_GetAngleDeg(const PotAngle_Handle *pot);

#ifdef __cplusplus
}
#endif

#endif /* POT_ANGLE_H */
