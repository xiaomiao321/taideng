/**
 * @file home_key.c
 * @brief 实体 HOME 键的消抖与上报调度实现。
 */

#include "home_key.h"

/** @brief 按当前状态填一份报告，序号加一。 */
static void fill_report(home_key_t *key, uint8_t kind,
                        mcu_home_key_report_t *report)
{
    report->input = key->is_pressed ? 1U : 0U;
    report->kind = kind;
    report->boot_id = key->boot_id;
    report->report_id = ++key->report_id;
    report->press_count = key->press_count;
}

void home_key_init(home_key_t *key, uint32_t boot_id, bool raw_pressed,
                   uint32_t now_ms)
{
    key->is_pressed = raw_pressed;
    key->is_changing = false;
    key->change_ms = now_ms;
    key->has_reported = false;
    key->snapshot_ms = now_ms;
    key->boot_id = boot_id;
    key->report_id = 0U;
    key->press_count = 0U;
}

bool home_key_update(home_key_t *key, bool raw_pressed, uint32_t now_ms,
                     mcu_home_key_report_t *report)
{
    if (raw_pressed == key->is_pressed)
    {
        key->is_changing = false;
    }
    else if (!key->is_changing)
    {
        key->is_changing = true;
        key->change_ms = now_ms;
    }

    /* 第一份报告必须是快照，按键事件留到下一次调用。 */
    if (!key->has_reported)
    {
        key->has_reported = true;
        key->snapshot_ms = now_ms;
        fill_report(key, MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT, report);
        return true;
    }

    if (key->is_changing &&
        ((uint32_t)(now_ms - key->change_ms) >= BSP_KEY_DEBOUNCE_MS))
    {
        key->is_changing = false;
        key->is_pressed = raw_pressed;
        if (raw_pressed)
        {
            ++key->press_count;
        }
        fill_report(key,
                    raw_pressed ? MCU_PROTOCOL_HOME_KEY_KIND_PRESSED
                                : MCU_PROTOCOL_HOME_KEY_KIND_RELEASED,
                    report);
        return true;
    }

    if ((uint32_t)(now_ms - key->snapshot_ms) >= HOME_KEY_SNAPSHOT_PERIOD_MS)
    {
        key->snapshot_ms = now_ms;
        fill_report(key, MCU_PROTOCOL_HOME_KEY_KIND_SNAPSHOT, report);
        return true;
    }
    return false;
}
