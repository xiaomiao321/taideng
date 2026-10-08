/**
 * @file bsp_buzzer.h
 * @brief 蜂鸣器板级支持包接口。
 *
 * 蜂鸣器由 TIM13_CH1 输出 PWM 驱动。调用本模块前，必须先完成
 * MX_TIM13_Init()。本模块只负责蜂鸣器的启停，不包含阻塞延时。
 */

#ifndef AITAID_BSP_BUZZER_H
#define AITAID_BSP_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 蜂鸣器接口状态。
 */
typedef enum
{
    BSP_BUZZER_STATUS_OK = 0,
    BSP_BUZZER_STATUS_NOT_INITIALIZED,
    BSP_BUZZER_STATUS_INVALID_ARGUMENT,
    BSP_BUZZER_STATUS_TIMER_ERROR
} bsp_buzzer_status_t;

/**
 * @brief 初始化蜂鸣器 BSP，并确保蜂鸣器处于关闭状态。
 *
 * @note 调用本函数前，必须先调用 MX_TIM13_Init()。
 *
 * @return bsp_buzzer_status_t 初始化结果。
 * @retval BSP_BUZZER_STATUS_OK 初始化成功。
 * @retval BSP_BUZZER_STATUS_TIMER_ERROR 定时器未正确初始化。
 */
bsp_buzzer_status_t bsp_buzzer_init(void);

/**
 * @brief 以指定频率启动蜂鸣器 PWM 输出。
 *
 * PWM 占空比固定为 50%。蜂鸣器在约 2.7 kHz 时通常声音最响，其他频率
 * 的音量可能有所下降。
 *
 * @param[in] frequency_hz 目标频率，单位为 Hz。
 *
 * @return bsp_buzzer_status_t 启动结果。
 * @retval BSP_BUZZER_STATUS_OK 启动成功。
 * @retval BSP_BUZZER_STATUS_NOT_INITIALIZED BSP 未初始化。
 * @retval BSP_BUZZER_STATUS_INVALID_ARGUMENT 频率为 0 或无法由定时器产生。
 * @retval BSP_BUZZER_STATUS_TIMER_ERROR PWM 启动失败。
 */
bsp_buzzer_status_t bsp_buzzer_start(uint32_t frequency_hz);

/**
 * @brief 停止蜂鸣器 PWM 输出。
 *
 * 重复调用本函数不会重复停止定时器，并返回成功。
 *
 * @return bsp_buzzer_status_t 停止结果。
 * @retval BSP_BUZZER_STATUS_OK 停止成功或蜂鸣器已经停止。
 * @retval BSP_BUZZER_STATUS_NOT_INITIALIZED BSP 未初始化。
 * @retval BSP_BUZZER_STATUS_TIMER_ERROR PWM 停止失败。
 */
bsp_buzzer_status_t bsp_buzzer_stop(void);

/**
 * @brief 获取蜂鸣器的软件运行状态。
 *
 * @param[out] is_active 用于接收蜂鸣器状态；true 表示 PWM 正在输出。
 *
 * @return bsp_buzzer_status_t 查询结果。
 * @retval BSP_BUZZER_STATUS_OK 查询成功。
 * @retval BSP_BUZZER_STATUS_NOT_INITIALIZED BSP 未初始化。
 * @retval BSP_BUZZER_STATUS_INVALID_ARGUMENT is_active 为空指针。
 */
bsp_buzzer_status_t bsp_buzzer_get_state(bool *is_active);

#endif /* AITAID_BSP_BUZZER_H */
