/**
 * @file bsp_led.h
 * @brief 板载状态 LED 的统一控制接口。
 */

#ifndef AITAID_BSP_LED_H
#define AITAID_BSP_LED_H

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
 * @brief LED 操作的返回状态。
 */
typedef enum
{
    BSP_LED_STATUS_OK = 0,
    BSP_LED_STATUS_INVALID_ID,
    BSP_LED_STATUS_INVALID_ARGUMENT
} bsp_led_status_t;

/**
 * @brief 初始化板载 LED 模块并关闭全部 LED。
 *
 * @note 调用本函数前必须已经执行 MX_GPIO_Init()。
 */
void bsp_led_init(void);

/**
 * @brief 设置指定 LED 的逻辑亮灭状态。
 *
 * @param[in] led_id LED 逻辑编号。
 * @param[in] is_on `true` 表示点亮，`false` 表示熄灭。
 *
 * @retval BSP_LED_STATUS_OK 设置成功。
 * @retval BSP_LED_STATUS_INVALID_ID LED 编号无效。
 *
 * @note 板载 LED 为低电平点亮，此硬件细节由实现层处理。
 */
bsp_led_status_t bsp_led_set(bsp_led_id_t led_id, bool is_on);

/**
 * @brief 翻转指定 LED 的逻辑亮灭状态。
 *
 * @param[in] led_id LED 逻辑编号。
 *
 * @retval BSP_LED_STATUS_OK 翻转成功。
 * @retval BSP_LED_STATUS_INVALID_ID LED 编号无效。
 */
bsp_led_status_t bsp_led_toggle(bsp_led_id_t led_id);

/**
 * @brief 读取指定 LED 当前的逻辑亮灭状态。
 *
 * @param[in] led_id LED 逻辑编号。
 * @param[out] is_on 接收 LED 状态；`true` 表示点亮，`false` 表示熄灭。
 *
 * @retval BSP_LED_STATUS_OK 读取成功。
 * @retval BSP_LED_STATUS_INVALID_ID LED 编号无效。
 * @retval BSP_LED_STATUS_INVALID_ARGUMENT `is_on` 为 NULL。
 */
bsp_led_status_t bsp_led_get_state(bsp_led_id_t led_id, bool *is_on);

#endif /* AITAID_BSP_LED_H */
