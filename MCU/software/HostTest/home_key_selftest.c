/**
 * @file home_key_selftest.c
 * @brief home_key 的 PC 端自测：消抖、按下松开事件、周期快照和各计数。
 *
 * @note 本文件不属于固件，不参与 arm 构建；输出一律 ASCII。
 */

#include <assert.h>
#include <stdio.h>

#include "home_key.h"

static home_key_t key;
static mcu_home_key_report_t report;

/** @brief 推进一次，返回是否有报告。 */
static bool step(bool raw, uint32_t now_ms)
{
    return home_key_update(&key, raw, now_ms, &report);
}

int main(void)
{
    /* 第一次调用给出快照，序号 1；没有变化时不再上报。 */
    home_key_init(&key, 7U, false, 1000U);
    assert(step(false, 1000U));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT);
    assert(report.input == 0U && report.boot_id == 7U);
    assert(report.report_id == 1U && report.press_count == 0U);
    assert(!step(false, 1001U));

    /* 按下：稳定满消抖时间才上报，计数加一。 */
    assert(!step(true, 1100U));
    assert(!step(true, 1100U + BSP_KEY_DEBOUNCE_MS - 1U));
    assert(step(true, 1100U + BSP_KEY_DEBOUNCE_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_PRESSED);
    assert(report.input == 1U && report.report_id == 2U);
    assert(report.press_count == 1U);
    assert(!step(true, 1200U));

    /* 松开过程中的抖动：弹回按下就重新计时，不产生事件。 */
    assert(!step(false, 1300U));
    assert(!step(true, 1310U));
    assert(!step(false, 1315U));
    assert(!step(false, 1315U + BSP_KEY_DEBOUNCE_MS - 1U));
    assert(step(false, 1315U + BSP_KEY_DEBOUNCE_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_RELEASED);
    assert(report.input == 0U && report.report_id == 3U);
    assert(report.press_count == 1U);

    /* 快照周期从上一份快照算起，与中间的按键事件无关。 */
    assert(!step(false, 1000U + HOME_KEY_SNAPSHOT_PERIOD_MS - 1U));
    assert(step(false, 1000U + HOME_KEY_SNAPSHOT_PERIOD_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT);
    assert(report.input == 0U && report.report_id == 4U);
    assert(report.press_count == 1U);

    /* 短于消抖时间的毛刺不上报，也不计数。 */
    assert(!step(true, 2100U));
    assert(!step(false, 2100U + BSP_KEY_DEBOUNCE_MS - 1U));
    assert(!step(false, 2200U));
    assert(key.press_count == 1U);

    /* 主循环被卡住很久：事件和到期的快照分两次给出，事件在前。 */
    assert(!step(true, 2900U));
    assert(step(true, 3500U));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_PRESSED);
    assert(report.report_id == 5U && report.press_count == 2U);
    assert(step(true, 3500U));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT);
    assert(report.input == 1U && report.report_id == 6U);

    /* 上电时已经按住：只有快照，input 为 1，不算按下；松开照常上报。 */
    home_key_init(&key, 8U, true, 0U);
    assert(step(true, 0U));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT);
    assert(report.input == 1U && report.boot_id == 8U);
    assert(report.report_id == 1U && report.press_count == 0U);
    assert(!step(true, 100U));
    assert(!step(false, 200U));
    assert(step(false, 200U + BSP_KEY_DEBOUNCE_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_RELEASED);
    assert(report.report_id == 2U && report.press_count == 0U);

    /* 初始化那一刻电平就变了：第一份仍是快照，事件随后给出。 */
    home_key_init(&key, 9U, false, 0U);
    assert(step(true, BSP_KEY_DEBOUNCE_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT);
    assert(report.input == 0U);
    assert(step(true, 2U * BSP_KEY_DEBOUNCE_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_PRESSED);

    /* 时刻回绕：消抖和快照周期都按无符号差值计算。 */
    home_key_init(&key, 10U, false, 0xFFFFFFF0U);
    assert(step(false, 0xFFFFFFF0U));
    assert(!step(true, 0xFFFFFFFAU));
    assert(step(true, 0xFFFFFFFAU + BSP_KEY_DEBOUNCE_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_PRESSED);
    assert(step(true, 0xFFFFFFF0U + HOME_KEY_SNAPSHOT_PERIOD_MS));
    assert(report.kind == MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT);

    puts("home_key: first snapshot, debounced press and release, bounce, "
         "glitch, periodic snapshot, held at boot, tick wrap passed");
    return 0;
}
