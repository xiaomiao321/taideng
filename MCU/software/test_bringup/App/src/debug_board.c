#include "debug_board.h"
#include "motor_limits.h"
#include <stdio.h>
#include <string.h>

const char *const debug_board_names[29] = {
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
    for (i = 0; i < 29; ++i)
    {
        if (!strcmp(name, debug_board_names[i]))
        {
            break;
        }
    }
    if (i == 29 || !duration || duration > 60000U)
    {
        return "TEST_PARAM";
    }
    board_kind_t kind = i < 6    ? (board_kind_t)i
                        : i < 13 ? BOARD_LAMP
                                 : BOARD_MOTOR;
    if ((kind == BOARD_BUZZER &&
         (value < 100 || value > 20000 || duration > 10000U)) ||
        (kind == BOARD_PWR && (value != 0 || duration > 10000U)) ||
        (kind == BOARD_LAMP && (value < 0 || value > 1000)) ||
        (kind == BOARD_MOTOR &&
         (value <= 0 || !motor_duty_valid((i - 13U) / 2U + 1U, value) ||
          duration > 5000U)) ||
        (kind < BOARD_BUZZER && value != 0) ||
        (kind == BOARD_ADC && value != 0))
    {
        return "TEST_PARAM";
    }
    debug_board_cancel(b);
    b->name = debug_board_names[i];
    b->seq = seq;
    b->kind = kind;
    b->index = kind == BOARD_LAMP    ? i - 6U
               : kind == BOARD_MOTOR ? (i - 13U) / 2U
                                     : 0U;
    b->value = kind == BOARD_MOTOR && (i - 13U) % 2U ? -value : value;
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
    if (b->phase == BOARD_SETTLE && (uint32_t)(now - b->started_ms) >= 100U)
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
        if (b->kind == BOARD_LED && ++b->led_step < 4U)
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
    static const char *const phases[] = {"IDLE", "SETTLE", "DRIVE", "WAIT",
                                         "KEY"};
    return phases[b->phase];
}
