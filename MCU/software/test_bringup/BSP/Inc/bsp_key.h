/**
 * @file bsp_key.h
 * @brief HOME 按键的原始电平轮询接口。
 */

#ifndef AITAID_BSP_KEY_H
#define AITAID_BSP_KEY_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief HOME 按键的软件消抖时间，单位为毫秒。
 *
 * @note 主板已经用 10kΩ 上拉和 100nF 电容构成约 1ms 的 RC 滤波。本参数是在
 *       硬件滤波之上再加的一层软件消抖，用于吸收被 RC 削弱、但仍跨过逻辑
 *       阈值的余抖，因此取值明显大于 RC 时间常数。
 */
#define BSP_KEY_DEBOUNCE_MS (20U)

/**
 * @brief HOME 按键读取操作的返回状态。
 */
typedef enum
{
    BSP_KEY_STATUS_OK = 0,
    BSP_KEY_STATUS_NOT_INITIALIZED,
    BSP_KEY_STATUS_INVALID_ARGUMENT
} bsp_key_status_t;

/**
 * @brief 初始化 HOME 按键模块的软件状态。
 *
 * @note 调用前必须已经执行 MX_GPIO_Init()；本函数不重新配置 GPIO 或 EXTI。
 * @note 重复调用无额外硬件动作，不产生按下事件；每次调用都会把按下计数清零，
 *       并重置消抖时间戳。
 * @note 本函数不等待按键动作，不调用 HAL_Delay()，不输出日志。
 * @warning 在主循环或任务上下文调用，不从 ISR 调用。
 * @warning 当前版本不保证初始化与读取并发调用安全。
 */
void bsp_key_init(void);

/**
 * @brief 读取 HOME 按键当前的原始按下状态，不进行软件消抖。
 *
 * @param[out] is_pressed 接收当前状态的指针，不允许为 NULL。
 *                        true 表示按下，false 表示松开。
 *
 * @retval BSP_KEY_STATUS_OK 读取成功，已写入 is_pressed。
 * @retval BSP_KEY_STATUS_NOT_INITIALIZED 模块尚未初始化，输出参数保持不变。
 * @retval BSP_KEY_STATUS_INVALID_ARGUMENT is_pressed 为 NULL。
 *
 * @note 同时存在未初始化和空指针时，优先返回 NOT_INITIALIZED。
 * @note HOME 为低电平有效，GPIO 引脚和电平转换由 .c 实现封装。
 * @note 本函数即时读取 GPIO，不保存传入的指针，不等待电平稳定，
 *       不调用 HAL_Delay()，不输出日志，也不修改 GPIO 或 EXTI 配置。
 * @note 返回的是当前状态，不是单次按下事件；持续按住时可以连续返回 true。
 * @warning 原始状态可能受机械抖动影响，不能直接当作消抖后的按下事件。
 * @warning 在主循环或任务上下文调用，不从 ISR 调用。
 * @warning 当前版本不保证初始化与读取并发调用安全。
 */
bsp_key_status_t bsp_key_read(bool *is_pressed);

/**
 * @brief 处理一次 HOME 按键下降沿，由 EXTI 中断服务程序调用。
 *
 * @note 通常由 HAL_GPIO_EXTI_Callback() 在确认参数引脚为 HOME_KEY_Pin 后转发，
 *       不要在中断之外的代码中调用。
 * @note 内部只读取 HAL_GetTick() 时间戳、比较并累加计数，不阻塞、不输出日志、
 *       不调用 HAL_Delay()，满足中断内保持短小的要求。
 * @note 与上一次被接受的下降沿间隔不足 BSP_KEY_DEBOUNCE_MS 的边沿会被丢弃。
 * @note 模块未初始化时本函数不做任何事，也不累加计数。
 * @warning 只能在 EXTI15_10 的中断上下文中调用。
 */
void bsp_key_on_exti_from_isr(void);

/**
 * @brief 读取自初始化以来被接受的按下事件累计次数。
 *
 * @param[out] press_count 接收累计次数，不允许为 NULL。
 *
 * @retval BSP_KEY_STATUS_OK 读取成功，已写入 press_count。
 * @retval BSP_KEY_STATUS_NOT_INITIALIZED 模块尚未初始化，输出参数保持不变。
 * @retval BSP_KEY_STATUS_INVALID_ARGUMENT press_count 为 NULL。
 *
 * @note 同时存在未初始化和空指针时，优先返回 NOT_INITIALIZED。
 * @note 计数只在 bsp_key_on_exti_from_isr() 中被累加，因此两次调用之间返回值
 *       可能被中断改变。调用者应与上一次读数比较来判断是否有新的按下，
 *       不要用返回值是否为 0 作为判断依据。
 * @note 计数只增不减，累加到 2^32 后回绕；比较时使用 != 而不是 >。
 * @note 本函数只读取软件计数，不访问 GPIO，不等待电平稳定，不输出日志。
 * @warning 在主循环或任务上下文调用，不从 ISR 调用。
 */
bsp_key_status_t bsp_key_get_press_count(uint32_t *press_count);

#endif /* AITAID_BSP_KEY_H */
