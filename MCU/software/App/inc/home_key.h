/**
 * @file home_key.h
 * @brief 实体 HOME 键的消抖与上报调度（HOME_KEY_REPORT 0x39）。
 *
 * 主循环每圈把按键的原始电平交给 home_key_update()，本模块做消抖，并决定此刻
 * 要不要发一份报告：初始化后立即发一份快照，此后每 HOME_KEY_SNAPSHOT_PERIOD_MS
 * 发一份快照，消抖后的按下和松开各立即发一份。
 *
 * 本模块只管上报，不停电机：HOME 键的本地急停仍由 EXTI 下降沿计数触发，
 * 与串口是否发得出去无关。
 *
 * @note 不依赖 HAL，可在 PC 上单测（HostTest/home_key_selftest.c）。
 */

#ifndef AITAID_HOME_KEY_H
#define AITAID_HOME_KEY_H

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "mcu_protocol.h"

/** @brief 上报状态。字段由本模块维护，调用者不得直接修改。 */
typedef struct
{
    bool is_pressed;           /**< 消抖后的逻辑状态。 */
    bool is_changing;          /**< 原始电平与逻辑状态不同，正在等它稳定。 */
    uint32_t change_ms;        /**< 原始电平开始与逻辑状态不同的时刻。 */
    bool has_reported;         /**< 是否已经发过第一份快照。 */
    uint32_t snapshot_ms;      /**< 上一份快照的时刻。 */
    uint32_t boot_id;          /**< 本次启动的标识。 */
    uint32_t report_id;        /**< 上一份报告的序号，第一份为 1。 */
    uint32_t press_count;      /**< 本次启动消抖后的按下次数。 */
} home_key_t;

/**
 * @brief 初始化。
 *
 * @param[in] boot_id 本次启动的标识，每次重启必须不同。
 * @param[in] raw_pressed 此刻按键的原始状态，直接作为初始逻辑状态。
 * @param[in] now_ms 当前时刻。
 *
 * @note 上电时已经按住只体现在快照里，不产生按下事件，也不计入按下次数。
 */
void home_key_init(home_key_t *key, uint32_t boot_id, bool raw_pressed,
                   uint32_t now_ms);

/**
 * @brief 主循环每圈调用：推进消抖，需要上报时填好一份报告。
 *
 * @param[in] raw_pressed 此刻按键的原始状态。
 * @param[in] now_ms 当前时刻。
 * @param[out] report 返回 true 时已填好，不允许为 NULL。
 *
 * @retval true 应当把 report 发给主机。
 * @retval false 此刻没有要发的报告，report 不变。
 *
 * @note 原始电平与逻辑状态持续不同满 BSP_KEY_DEBOUNCE_MS 才算一次变化，中途
 *       弹回去就重新计时。
 * @note 每次调用最多给出一份报告，按下和松开优先于快照；每份报告的序号加一。
 */
bool home_key_update(home_key_t *key, bool raw_pressed, uint32_t now_ms,
                     mcu_home_key_report_t *report);

#endif /* AITAID_HOME_KEY_H */
