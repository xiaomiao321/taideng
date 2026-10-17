/**
 * @file bsp_led.h
 * @brief 板载状态 LED 的统一控制接口。
 */

#ifndef AITAID_BSP_LED_H
#define AITAID_BSP_LED_H

#include "bsp_common.h"

#include <stdbool.h>

/**
 * @brief 板载 LED 的逻辑编号。
 *
 * 调用者只使用逻辑编号，不依赖具体 GPIO 引脚和有效电平。
 */
typedef enum
{
    BSP_LED_ID_STATE = 0,
    BSP_LED_ID_COMM,
    BSP_LED_ID_ACTIVITY,
    BSP_LED_ID_ERROR,
    BSP_LED_ID_COUNT
} bsp_led_id_t;

/**
 * @brief 初始化板载 LED 模块并关闭全部 LED。
 *
 * @retval BSP_OK 初始化成功；当前无硬件校验，始终返回成功。
 *
 * @note 调用本函数前必须已经执行 MX_GPIO_Init()。
 * @note 与其他 BSP 模块不同，本模块没有"未初始化"状态：bsp_led_set() 等
 *       接口在 bsp_led_init() 之前也可调用，这样 Error_Handler() 在任何阶段
 *       都能点亮错误灯。bsp_led_init() 只负责把全部 LED 置为熄灭。
 */
bsp_status_t bsp_led_init(void);

/**
 * @brief 设置指定 LED 的逻辑亮灭状态。
 *
 * @param[in] led_id LED 逻辑编号。
 * @param[in] is_on `true` 表示点亮，`false` 表示熄灭。
 *
 * @retval BSP_OK 设置成功。
 * @retval BSP_ERR_PARAM LED 编号无效。
 *
 * @note 板载 LED 为低电平点亮，此硬件细节由实现层处理。
 */
bsp_status_t bsp_led_set(bsp_led_id_t led_id, bool is_on);

/**
 * @brief 翻转指定 LED 的逻辑亮灭状态。
 *
 * @param[in] led_id LED 逻辑编号。
 *
 * @retval BSP_OK 翻转成功。
 * @retval BSP_ERR_PARAM LED 编号无效。
 */
bsp_status_t bsp_led_toggle(bsp_led_id_t led_id);

/**
 * @brief 读取指定 LED 当前的逻辑亮灭状态。
 *
 * @param[in] led_id LED 逻辑编号。
 * @param[out] is_on 接收 LED 状态；`true` 表示点亮，`false` 表示熄灭。
 *
 * @retval BSP_OK 读取成功。
 * @retval BSP_ERR_PARAM LED 编号无效，或 `is_on` 为 NULL。
 */
bsp_status_t bsp_led_get_state(bsp_led_id_t led_id, bool *is_on);

#endif /* AITAID_BSP_LED_H */
