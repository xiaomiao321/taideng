/**
  ******************************************************************************
  * @file    pot_angle.c
  * @brief   电位器角度采样库实现 (连采平均 + 一阶 IIR + 整数角度换算)
  *
  * 设计要点:
  *   1. 全部运算为整数 (无浮点): M3 无 FPU, 整数实现更小更快;
  *   2. 角度 = (filtered - raw_at_0deg) × 满量程 / span, 四舍五入 (加 span/2),
  *      并限幅到 0 ~ 满量程, 端点附近有噪声/超调也不会出现负角度;
  *   3. IIR 用 "加半再右移" 实现 1/2^shift 步进: 小差值时也能收敛,
  *      不会因整数截断卡死在目标值附近; shift=0 时旁路 (最低延迟);
  *   4. 转换失败 (超时) 的样本自动丢弃; 整帧全失败则保留上一帧输出;
  *   5. PotAngle_Init() 内含 ADC 上电校准, 并在最后先采一帧, 让 IIR 从
  *      真实值起步 (上电即稳定, 不需要等收敛过程).
  ******************************************************************************
  */

#include "pot_angle.h"

/* 说明: 公开接口的完整文档 (参数/返回值/用法) 在 pot_angle.h 中;
   本文件保留实现细节注释与私有函数说明. */

/* -------------------------------------------------------------------------- */
/* 私有定义                                                                    */
/* -------------------------------------------------------------------------- */

/** @brief 单次转换超时 (ms): 71.5 周期 @12MHz 约 7µs, 2ms 已极其宽裕 */
#define POT_ANGLE_ADC_TIMEOUT_MS 2U

/** @brief 连采次数上限 (帧内忙等时间上限约 0.26ms) */
#define POT_ANGLE_BURST_MAX 32U

/** @brief IIR 系数上限 (2^8 = 256, 时间常数约 0.26s @1kHz, 足够慢) */
#define POT_ANGLE_EMA_SHIFT_MAX 8U

/* -------------------------------------------------------------------------- */
/* 私有函数                                                                    */
/* -------------------------------------------------------------------------- */

/**
  * @brief  一阶 IIR (指数平均): current 向 target 平滑趋近 1/2^shift
  * @param  current 当前滤波值
  * @param  target  新采样值
  * @param  shift   系数 (0 ~ 8): 0 = 旁路 (直接采用 target, 最低延迟),
  *                 越大越平滑 (时间常数 ≈ 2^shift / 采样率)
  * @retval 趋近后的新滤波值
  * @note   加半再右移 = 四舍五入, 避免小差值时右移结果为 0 而卡死.
  */
static uint16_t pot_angle_iir(uint16_t current, uint16_t target, uint8_t shift)
{
    int32_t diff;

    if (shift == 0U)
    {
        return target; /* 关闭滤波: 本帧平均直接作为输出 */
    }

    diff = (int32_t)target - (int32_t)current;

    return (uint16_t)((int32_t)current +
           ((diff + (1 << (shift - 1U))) >> shift));
}

/**
  * @brief  raw -> 整数角度: 线性映射 + 四舍五入 + 端点限幅
  * @param  raw            原始滤波值
  * @param  raw_at_0deg    0° 端点标定 raw
  * @param  raw_at_270deg  满量程端点标定 raw (必须 > raw_at_0deg)
  * @retval 0 ~ POT_ANGLE_FULL_DEG 的整数角度
  * @note   端点标定非法 (span <= 0) 时输出 0, 避免除零.
  */
static uint16_t pot_angle_map_deg(uint16_t raw, uint16_t raw_at_0deg, uint16_t raw_at_270deg)
{
    int32_t span = (int32_t)raw_at_270deg - (int32_t)raw_at_0deg;
    int32_t deg;

    if (span <= 0)
    {
        return 0U;
    }

    deg = (((int32_t)raw - (int32_t)raw_at_0deg) * (int32_t)POT_ANGLE_FULL_DEG + span / 2) / span;

    if (deg < 0)
    {
        deg = 0;
    }
    if (deg > (int32_t)POT_ANGLE_FULL_DEG)
    {
        deg = (int32_t)POT_ANGLE_FULL_DEG;
    }

    return (uint16_t)deg;
}

/* -------------------------------------------------------------------------- */
/* 公开接口实现                                                                */
/* -------------------------------------------------------------------------- */

bool PotAngle_Init(PotAngle_Handle *pot, ADC_HandleTypeDef *hadc)
{
    if ((pot == NULL) || (hadc == NULL))
    {
        return false;
    }

    pot->hadc = hadc;
    pot->raw_at_0deg = POT_ANGLE_RAW_AT_0DEG;
    pot->raw_at_270deg = POT_ANGLE_RAW_AT_270DEG;
    pot->burst_samples = POT_ANGLE_BURST_SAMPLES;
    pot->ema_shift = POT_ANGLE_EMA_SHIFT;

    pot->raw = 0U;
    pot->avg = 0U;
    pot->filtered = 0U;
    pot->angle_deg = 0U;

    /* ADC 上电校准 (F1 必须, 且必须在任何一次转换之前) */
    if (HAL_ADCEx_Calibration_Start(hadc) != HAL_OK)
    {
        return false;
    }

    /* 先采一帧: 让 IIR 从真实值起步, 上电即稳定 */
    PotAngle_Update(pot);

    return true;
}

void PotAngle_SetEndpoints(PotAngle_Handle *pot, uint16_t raw_at_0deg, uint16_t raw_at_270deg)
{
    if (pot == NULL)
    {
        return;
    }

    pot->raw_at_0deg = raw_at_0deg;
    pot->raw_at_270deg = raw_at_270deg;
}

bool PotAngle_SetFilter(PotAngle_Handle *pot, uint8_t burst_samples, uint8_t ema_shift)
{
    if (pot == NULL)
    {
        return false;
    }
    if ((burst_samples < 1U) || (burst_samples > POT_ANGLE_BURST_MAX))
    {
        return false;
    }
    if (ema_shift > POT_ANGLE_EMA_SHIFT_MAX)
    {
        return false;
    }

    pot->burst_samples = burst_samples;
    pot->ema_shift = ema_shift;

    return true;
}

void PotAngle_Update(PotAngle_Handle *pot)
{
    uint32_t sum = 0U;
    uint32_t count = 0U;
    uint16_t last = 0U;

    if ((pot == NULL) || (pot->hadc == NULL))
    {
        return;
    }

    /* ---- 第 1 级: 连采 N 次求平均 ---- */
    for (uint32_t i = 0U; i < (uint32_t)pot->burst_samples; i++)
    {
        HAL_ADC_Start(pot->hadc);
        if (HAL_ADC_PollForConversion(pot->hadc, POT_ANGLE_ADC_TIMEOUT_MS) == HAL_OK)
        {
            last = (uint16_t)HAL_ADC_GetValue(pot->hadc);
            sum += last;
            count++;
        }
        HAL_ADC_Stop(pot->hadc);
    }

    if (count == 0U)
    {
        return; /* 整帧转换全部失败: 保留上一帧输出, 等待下次调用 */
    }

    pot->raw = last;
    pot->avg = (uint16_t)(sum / count);

    /* ---- 第 2 级: 一阶 IIR (指数平均) ---- */
    pot->filtered = pot_angle_iir(pot->filtered, pot->avg, pot->ema_shift);

    /* ---- 换算整数角度 ---- */
    pot->angle_deg = pot_angle_map_deg(pot->filtered, pot->raw_at_0deg, pot->raw_at_270deg);
}

uint16_t PotAngle_GetRaw(const PotAngle_Handle *pot)
{
    return (pot != NULL) ? pot->raw : 0U;
}

uint16_t PotAngle_GetAvg(const PotAngle_Handle *pot)
{
    return (pot != NULL) ? pot->avg : 0U;
}

uint16_t PotAngle_GetFiltered(const PotAngle_Handle *pot)
{
    return (pot != NULL) ? pot->filtered : 0U;
}

uint16_t PotAngle_GetAngleDeg(const PotAngle_Handle *pot)
{
    return (pot != NULL) ? pot->angle_deg : 0U;
}
