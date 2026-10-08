/**
 * @file bsp_linux_power.h
 * @brief Linux 电源键脉冲的非阻塞板级接口。
 *
 * 本模块只生成电源键脉冲，不判断 Linux 是否已经开机或关机。
 * 所有接口由同一个主循环或任务串行调用，不可重入，不从 ISR 调用。
 * 本模块不使用 HAL_Delay()，不输出日志。
 */

#ifndef AITAID_BSP_LINUX_POWER_H
#define AITAID_BSP_LINUX_POWER_H

#include <stdint.h>

/**
 * @brief 当前 Bring-up 版本允许的最大脉冲持续时间，单位为毫秒。
 * @note 这是软件参数上限，不代表 Linux 核心板要求的电源键时序。
 */
#define BSP_LINUX_POWER_MAX_DURATION_MS (10000U)

/**
 * @brief Linux 电源键脉冲操作的返回状态。
 */
typedef enum
{
    BSP_LINUX_POWER_STATUS_OK = 0,
    BSP_LINUX_POWER_STATUS_NOT_INITIALIZED,
    BSP_LINUX_POWER_STATUS_INVALID_ARGUMENT,
    BSP_LINUX_POWER_STATUS_BUSY
} bsp_linux_power_status_t;

/**
 * @brief 初始化软件状态并释放 Linux 电源键。
 *
 * @note 调用前必须已经执行 MX_GPIO_Init()，本函数不重新配置 GPIO。
 * @note 将 PB12 输出置低，使外部 Q10 关闭，LINUX_PWR_OD 处于释放状态。
 * @note 每次调用都恢复空闲状态并清除脉冲计时信息；若已有脉冲，立即结束。
 * @note 释放是高阻态，Linux 接口侧的高电平由外部上拉提供。
 * @note 本函数立即返回，不等待外部设备响应。
 */
void bsp_linux_power_init(void);

/**
 * @brief 立即开始一次电源键脉冲，并返回主循环继续执行。
 *
 * @param[in] duration_ms 按键保持时间，单位为毫秒，
 *                       范围为 1 至 BSP_LINUX_POWER_MAX_DURATION_MS。
 *
 * @retval BSP_LINUX_POWER_STATUS_OK 已启动脉冲，不代表 Linux 已完成开关机。
 * @retval BSP_LINUX_POWER_STATUS_NOT_INITIALIZED 模块尚未初始化。
 * @retval BSP_LINUX_POWER_STATUS_INVALID_ARGUMENT duration_ms 超出范围。
 * @retval BSP_LINUX_POWER_STATUS_BUSY 上一次脉冲仍在进行。
 *
 * @note 检查顺序为初始化、参数范围、忙状态；失败时不改变输出及计时状态。
 * @note 成功时记录 HAL_GetTick() 和持续时间，将 PB12 置高，使 Q10 导通，
 *       将 Linux 接口侧的 LINUX_PWR_OD 拉低，进入脉冲进行状态。
 * @note 忙状态下不会重新计时，也不会延长或缩短当前脉冲。
 * @warning 调用后必须持续调用 bsp_linux_power_update() 才能自动释放按键。
 * @warning 实际连接 Linux 核心板前必须确认其电气要求及脉冲时长。
 */
bsp_linux_power_status_t bsp_linux_power_start(uint32_t duration_ms);

/**
 * @brief 推进一次脉冲状态处理，到达持续时间后释放电源键。
 *
 * @retval BSP_LINUX_POWER_STATUS_OK 本次处理完成，包括空闲或脉冲尚未到期。
 * @retval BSP_LINUX_POWER_STATUS_NOT_INITIALIZED 模块尚未初始化，不改变输出。
 *
 * @note 空闲时直接返回；脉冲进行时比较当前 tick 与开始 tick 的无符号差值，
 *       差值大于或等于 duration_ms 时将 PB12 置低并恢复空闲状态。
 * @note 用 uint32_t 无符号减法计算经过时间，不直接比较开始时间加持续时间。
 * @note OK 不表示脉冲一定已经结束，也不表示 Linux 的电源状态。
 * @warning 实际脉冲在到期后的第一次 update 调用时结束，调用间隔和调度延迟
 *          会造成脉冲延长；建议 Bring-up 主循环每圈调用，避免长时间阻塞。
 * @warning 停止调用本函数时，已经拉低的 Linux 电源键不会自行释放。
 */
bsp_linux_power_status_t bsp_linux_power_update(void);

#endif /* AITAID_BSP_LINUX_POWER_H */
