#include "debug_board.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool off_ok = true, apply_ok = true;
static unsigned applied, off_count, index_seen;
static int value_seen;
static char reply[100];
static bool apply(board_kind_t kind, unsigned index, int value)
{
    (void)kind;
    ++applied;
    index_seen = index;
    value_seen = value;
    return apply_ok;
}
static bool off(void)
{
    ++off_count;
    return off_ok;
}
static void emit(const char *s)
{
    snprintf(reply, sizeof(reply), "%s", s);
}
static debug_board_t fresh(void)
{
    debug_board_t b;
    board_hw_t hw = {apply, off, emit};
    applied = off_count = 0;
    apply_ok = off_ok = true;
    reply[0] = 0;
    debug_board_init(&b, hw);
    return b;
}
int main(void)
{
    debug_board_t b;
    /* Every table entry must be recognized and runnable with defaults. */
    for (unsigned i = 0; i < DEBUG_BOARD_TEST_COUNT; ++i)
    {
        b = fresh();
        for (unsigned j = 0; j < i; ++j)
        {
            assert(strcmp(debug_board_names[i], debug_board_names[j]));
        }
        int value = i == 3 ? 2700 : i >= 13 ? 800 : i >= 6 ? 500 : 0;
        assert(!debug_board_start(&b, 42, debug_board_names[i], value, 500, 0));
        debug_board_update(&b, 99, false);
        assert(applied == 0);
        debug_board_update(&b, 100, false);
        assert(applied == 1);
        if (i < 6)
        {
            if (i == 0 || i == 5)
            {
                assert(strstr(reply, "PASS"));
            }
            else if (i == 2)
            {
                debug_board_update(&b, 101, true);
                assert(strstr(reply, "PASS"));
            }
            else if (i == 1)
            {
                for (unsigned step = 1; step < 4; ++step)
                {
                    debug_board_update(&b, 100 + step * 500, false);
                    assert(index_seen == step);
                }
                debug_board_update(&b, 2100, false);
            }
            else
            {
                debug_board_update(&b, 600, false);
            }
        }
        else
        {
            if (i >= 13)
            {
                assert(index_seen == (i - 13) / 2);
                assert(value_seen == ((i - 13) % 2 ? -800 : 800));
            }
            else
            {
                assert(index_seen == i - 6);
            }
            debug_board_update(&b, 599, false);
            assert(b.phase == BOARD_DRIVE);
            debug_board_update(&b, 600, false);
        }
        if (b.phase == BOARD_WAIT)
        {
            assert(off_count > 0);
            assert(!debug_board_confirm(&b, true));
            assert(strstr(reply, "RESULT 42") && strstr(reply, "PASS"));
        }
        assert(b.phase == BOARD_IDLE);
    }
    b = fresh();
    assert(!debug_board_start(&b, 1, "KEY", 0, 500, 0));
    debug_board_update(&b, 100, false);
    debug_board_update(&b, 600, false);
    assert(strstr(reply, "FAIL_NO_RESPONSE"));
    b = fresh();
    assert(!debug_board_start(&b, 2, "BUZZER", 2700, 100, 0));
    assert(debug_board_confirm(&b, true));
    debug_board_update(&b, 100, false);
    debug_board_update(&b, 200, false);
    assert(!debug_board_confirm(&b, false));
    assert(strstr(reply, "FAIL_MANUAL"));
    b = fresh();
    assert(!debug_board_start(&b, 3, "MOTOR_M1_FI", 800, 500, 0));
    debug_board_cancel(&b);
    debug_board_update(&b, 100, false);
    assert(applied == 0 && strstr(reply, "CANCELLED"));
    b = fresh();
    apply_ok = false;
    assert(!debug_board_start(&b, 4, "ADC6", 0, 500, 0));
    debug_board_update(&b, 100, false);
    assert(strstr(reply, "FAIL_COMM"));
    b = fresh();
    off_ok = false;
    assert(!debug_board_start(&b, 5, "PWR", 0, 200, 0));
    debug_board_update(&b, 100, false);
    debug_board_update(&b, 300, false);
    assert(strstr(reply, "FAIL_COMM"));
    b = fresh();
    assert(debug_board_start(&b, 1, "UNKNOWN", 0, 500, 0));
    assert(debug_board_start(&b, 1, "MOTOR_M1_FI", 0, 500, 0));
    assert(debug_board_start(&b, 1, "MOTOR_M1_FI", 800, 5001, 0));
    assert(debug_board_start(&b, 1, "BUZZER", 99, 500, 0));
    assert(debug_board_start(&b, 1, "LAMP_UP1", 1001, 500, 0));
    assert(debug_board_start(&b, 1, "PWR", 0, 10001, 0));
    assert(debug_board_start(&b, 1, "LED", 1, 500, 0));
    assert(debug_board_start(&b, 1, "MOTOR_M1_FI", 499, 500, 0));
    assert(debug_board_start(&b, 1, "MOTOR_M7_FI", 801, 500, 0));
    assert(debug_board_start(&b, 1, "MOTOR_M8_BI", 1000, 500, 0));
    assert(b.phase == BOARD_IDLE);
    assert(
        !debug_board_start(&b, 7, "MOTOR_M8_BI", 800, 200, UINT32_MAX - 150U));
    debug_board_update(&b, UINT32_MAX - 50U, false);
    assert(b.phase == BOARD_DRIVE);
    debug_board_update(&b, 149U, false);
    assert(b.phase == BOARD_WAIT);
    puts("board tests: 29 cases, timing, confirmation, cancellation, errors "
         "and rollover passed");
    return 0;
}
