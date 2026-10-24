#include "debug_board.h"

#include <stdio.h>
#include <string.h>

/** @brief 接受测试后、驱动输出前的等待时间，单位毫秒。 */
#define SETTLE_MS (100U)

/** @brief 任何测试的最长持续时间，单位毫秒。 */
#define MAX_DURATION_MS (60000U)

/** @brief 蜂鸣器与 Linux 电源脉冲测试的最长持续时间，单位毫秒。 */
#define MAX_PULSE_DURATION_MS (10000U)

/** @brief 蜂鸣器测试频率范围，单位 Hz。 */
#define BUZZER_MIN_HZ (100)
#define BUZZER_MAX_HZ (20000)

/** @brief 灯板测试占空比上限，千分比。 */
#define LAMP_MAX_PERMILLE (1000)

const char *const debug_board_names[DEBUG_BOARD_TEST_COUNT] = {
    "LOG",         "LED",         "KEY",         "BUZZER",      "PWR",
    "ADC6",        "LAMP_UP1",    "LAMP_UP2",    "LAMP_UP3",    "LAMP_UP4",
    "LAMP_DOWN1",  "LAMP_DOWN2",  "LAMP_DOWN3",  "MOTOR_M1_FI", "MOTOR_M1_BI",
    "MOTOR_M2_FI", "MOTOR_M2_BI", "MOTOR_M3_FI", "MOTOR_M3_BI", "MOTOR_M4_FI",
    "MOTOR_M4_BI", "MOTOR_M5_FI", "MOTOR_M5_BI", "MOTOR_M6_FI", "MOTOR_M6_BI",
    "MOTOR_M7_FI", "MOTOR_M7_BI", "MOTOR_M8_FI", "MOTOR_M8_BI"};
void debug_board_init(debug_board_t *b, board_hw_t hw)
{
    memset(b, 0, sizeof(*b));
    b->hw = hw;
    b->name = "-";
}
static void finish(debug_board_t *b, const char *result)
{
    char out[96];
    b->phase = BOARD_IDLE;
    if (!b->hw.off())
    {
        result = "FAIL_COMM";
    }
    (void)snprintf(out, sizeof(out), "RESULT %u %s %s\r\n", b->seq, b->name,
                   result);
    b->hw.reply(out);
}
void debug_board_cancel(debug_board_t *b)
{
    if (b->phase != BOARD_IDLE)
    {
        finish(b, "CANCELLED");
    }
}
const char *debug_board_start(debug_board_t *b, unsigned seq, const char *name,
                              int value, uint32_t duration, uint32_t now)
{
    unsigned i;
    for (i = 0; i < DEBUG_BOARD_TEST_COUNT; ++i)
    {
        if (!strcmp(name, debug_board_names[i]))
        {
            break;
        }
    }
    if (i == DEBUG_BOARD_TEST_COUNT || !duration || duration > MAX_DURATION_MS)
    {
        return "TEST_PARAM";
    }
    board_kind_t kind = (i < DEBUG_BOARD_LAMP_FIRST)    ? (board_kind_t)i
                        : (i < DEBUG_BOARD_MOTOR_FIRST) ? BOARD_LAMP
                                                        : BOARD_MOTOR;
    /* 电机测试每台占两项：偶数项 FI（正转），奇数项 BI（反转）。 */
    unsigned motor_item =
        (kind == BOARD_MOTOR) ? i - DEBUG_BOARD_MOTOR_FIRST : 0U;
    unsigned motor = motor_item / 2U;
    bool is_reverse = (motor_item % 2U) != 0U;
    if ((kind == BOARD_BUZZER &&
         (value < BUZZER_MIN_HZ || value > BUZZER_MAX_HZ ||
          duration > MAX_PULSE_DURATION_MS)) ||
        (kind == BOARD_PWR &&
         (value != 0 || duration > MAX_PULSE_DURATION_MS)) ||
        (kind == BOARD_LAMP && (value < 0 || value > LAMP_MAX_PERMILLE)) ||
        (kind == BOARD_MOTOR &&
         (value <= 0 || !motor_duty_valid(motor, value) ||
          duration > DEBUG_BOARD_MAX_MOTOR_DURATION_MS)) ||
        (kind < BOARD_BUZZER && value != 0) ||
        (kind == BOARD_ADC && value != 0))
    {
        return "TEST_PARAM";
    }
    debug_board_cancel(b);
    b->name = debug_board_names[i];
    b->seq = seq;
    b->kind = kind;
    b->index = (kind == BOARD_LAMP)    ? i - DEBUG_BOARD_LAMP_FIRST
               : (kind == BOARD_MOTOR) ? motor
                                       : 0U;
    b->value = (kind == BOARD_MOTOR && is_reverse) ? -value : value;
    b->duration_ms = duration;
    b->started_ms = now;
    b->led_step = 0;
    b->phase = BOARD_SETTLE;
    return NULL;
}
const char *debug_board_confirm(debug_board_t *b, bool pass)
{
    if (b->phase != BOARD_WAIT)
    {
        return "NOT_WAITING";
    }
    finish(b, pass ? "PASS" : "FAIL_MANUAL");
    return NULL;
}
void debug_board_update(debug_board_t *b, uint32_t now, bool home)
{
    if (b->phase == BOARD_SETTLE &&
        (uint32_t)(now - b->started_ms) >= SETTLE_MS)
    {
        if (!b->hw.apply(b->kind, b->index, b->value))
        {
            finish(b, "FAIL_COMM");
            return;
        }
        b->started_ms = now;
        if (b->kind == BOARD_LOG || b->kind == BOARD_ADC)
        {
            finish(b, "PASS");
            return;
        }
        b->phase = b->kind == BOARD_KEY ? BOARD_KEY_WAIT : BOARD_DRIVE;
    }
    if (b->phase == BOARD_KEY_WAIT)
    {
        if (home)
        {
            finish(b, "PASS");
        }
        else if ((uint32_t)(now - b->started_ms) >= b->duration_ms)
        {
            finish(b, "FAIL_NO_RESPONSE");
        }
    }
    if (b->phase == BOARD_DRIVE &&
        (uint32_t)(now - b->started_ms) >= b->duration_ms)
    {
        if (!b->hw.off())
        {
            finish(b, "FAIL_COMM");
            return;
        }
        if (b->kind == BOARD_LED && ++b->led_step < DEBUG_BOARD_LED_COUNT)
        {
            if (!b->hw.apply(BOARD_LED, b->led_step, 0))
            {
                finish(b, "FAIL_COMM");
                return;
            }
            b->started_ms = now;
        }
        else
        {
            b->phase = BOARD_WAIT;
            char out[80];
            (void)snprintf(out, sizeof(out), "TESTWAIT %u %s\r\n", b->seq,
                           b->name);
            b->hw.reply(out);
        }
    }
}
const char *debug_board_phase(const debug_board_t *b)
{
    static const char *const phases[BOARD_PHASE_COUNT] = {
        "IDLE", "SETTLE", "DRIVE", "WAIT", "KEY"};
    return ((unsigned)b->phase < BOARD_PHASE_COUNT) ? phases[b->phase] : "?";
}
