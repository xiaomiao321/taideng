/**
 * @file bsp_watchdog.h
 * @brief 独立看门狗（IWDG）：主循环卡死时复位芯片，复位后电机输出回到滑行。
 *
 * 直接操作 IWDG 寄存器，不依赖 HAL 的 IWDG 模块，因此不需要在 CubeMX 中
 * 启用 IWDG，重新生成代码也不会覆盖本模块。
 *
 * @note IWDG 一旦启动，只有复位能停止它。
 * @note 调试器暂停内核时 IWDG 同步暂停（DBGMCU 冻结位），断点调试不会被复位。
 * @note IWDG 时钟为 LSI，标称 32kHz，实际 17～47kHz；实际超时约为
 *       设定值的 0.68～1.9 倍，设定时要给主循环留足余量。
 */

#ifndef AITAID_BSP_WATCHDOG_H
#define AITAID_BSP_WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

#include "bsp_common.h"

/** @brief 允许设定的最大超时，单位为毫秒（标称值，预分频 /256 时的上限）。 */
#define BSP_WATCHDOG_MAX_TIMEOUT_MS (32768U)

/**
 * @brief 启动看门狗。
 *
 * @param[in] timeout_ms 标称超时，范围为 1 至 BSP_WATCHDOG_MAX_TIMEOUT_MS。
 *
 * @retval BSP_OK 已启动。
 * @retval BSP_ERR_PARAM 超时超出范围，看门狗未启动。
 * @retval BSP_ERR_TIMEOUT 预分频或重装载值写入后未在限定时间内生效；
 *         看门狗已以未知超时运行，调用者应按致命错误处理。
 *
 * @note 启动后必须周期调用 bsp_watchdog_feed()，间隔小于 timeout_ms 的 0.68 倍。
 */
bsp_status_t bsp_watchdog_start(uint32_t timeout_ms);

/**
 * @brief 修改运行中看门狗的超时，并立即喂狗。
 *
 * 供已知会长时间阻塞的操作（如擦除 Flash 扇区）临时放宽超时，结束后再用
 * bsp_watchdog_timeout() 记下的原值恢复。
 *
 * @retval BSP_OK 已修改。
 * @retval BSP_IDLE 看门狗没有启动，什么都不做。
 * @retval BSP_ERR_PARAM 超时超出范围。
 * @retval BSP_ERR_TIMEOUT 新值未在限定时间内生效。
 */
bsp_status_t bsp_watchdog_set_timeout(uint32_t timeout_ms);

/** @brief 当前设定的标称超时；看门狗未启动时返回 0。 */
uint32_t bsp_watchdog_timeout(void);

/**
 * @brief 喂狗，重新开始超时计数。
 *
 * @note 只写一个寄存器，开销可忽略；未启动时调用无副作用。
 */
void bsp_watchdog_feed(void);

/**
 * @brief 查询上一次复位是否由看门狗引起，并清除全部复位标志。
 *
 * @retval true 上一次复位来自 IWDG。
 * @retval false 其他原因复位，或标志已被清除。
 *
 * @note 会清除 RCC_CSR 中全部复位原因标志，只应在启动时调用一次。
 */
bool bsp_watchdog_take_reset_flag(void);

#endif /* AITAID_BSP_WATCHDOG_H */
