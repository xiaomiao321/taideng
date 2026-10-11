#include "debug_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned running_motor, stops;
static int running_duty;
static bool stop_ok = true, run_ok = true;
static char reply[128];
static bool hw_stop(void) { ++stops; running_motor = 0; running_duty = 0; return stop_ok; }
static bool hw_run(unsigned motor, int duty) { running_motor = motor; running_duty = duty; return run_ok; }
static void hw_reply(const char *s) { snprintf(reply, sizeof(reply), "%s", s); }
static debug_control_t fresh(uint32_t now)
{
    debug_control_t c;
    debug_hw_t hw = {hw_stop, hw_run, hw_reply};
    stop_ok = true; run_ok = true;
    debug_control_init(&c, hw);
    debug_control_line(&c, "PING 1", now);
    return c;
}
int main(void)
{
    debug_control_t c = fresh(0);
    debug_control_line(&c, "RUN 2 M1 800 500", 0);
    assert(c.pending && running_motor == 0);
    debug_control_update(&c, 99, false); assert(running_motor == 0);
    debug_control_update(&c, 100, false); assert(running_motor == 1 && running_duty == 800);
    debug_control_update(&c, 599, false); assert(running_motor == 1);
    debug_control_update(&c, 600, false); assert(running_motor == 0 && !c.locked);

    c = fresh(100);
    debug_control_line(&c, "RUN 2 M8 -1000 0", 100);
    debug_control_update(&c, 200, false); assert(running_motor == 8 && running_duty == -1000);
    debug_control_line(&c, "RUN 3 M1 800 500", 210); assert(running_motor == 0);
    debug_control_update(&c, 309, false); assert(running_motor == 0);
    debug_control_update(&c, 310, false); assert(running_motor == 1);
    debug_control_update(&c, 320, true); assert(c.locked && !c.pending && running_motor == 0);
    debug_control_line(&c, "RUN 4 M1 800 500", 321); assert(strstr(reply, "LOCKED"));
    debug_control_line(&c, "STOP 5 ALL", 322); assert(c.locked);
    debug_control_update(&c, 330, false);
    debug_control_line(&c, "STOP 6 ALL", 331); assert(!c.locked);

    c = fresh(0);
    debug_control_line(&c, "RUN 2 M1 800 0", 0);
    debug_control_update(&c, 100, false);
    debug_control_update(&c, 1000, false); assert(c.locked && running_motor == 0 && !c.linked);
    debug_control_line(&c, "PING 3", 1010);
    debug_control_line(&c, "RUN 4 M1 800 0", 1011); assert(!c.pending);
    debug_control_line(&c, "STOP 5 ALL", 1012); assert(!c.locked);

    const char *bad[] = {"RUN 2 M0 800 500", "RUN 2 M9 800 500", "RUN 2 M1 1001 500",
        "RUN 2 M1 0 500", "RUN 2 M1 800 -1", "RUN 2 M1 800 60001", "RUN 2 M1 80x 500",
        "RUN 2 M1 800 500 extra", "RUN 65536 M1 800 500", "RUN 2 M1 99999999999999999999999 500",
        "RUN 2 M1 +800 500", "PING 3 extra", "RUN"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        c = fresh(0); debug_control_line(&c, bad[i], 1);
        assert(!c.pending && !strncmp(reply, "ERR", 3));
    }
    c = fresh(0); debug_control_line(&c, "RUN 2 M1 800 0", 1000);
    assert(strstr(reply, "NO_HEARTBEAT"));
    c = fresh(0); debug_control_line(&c, "RUN 2 M1 800 0", 0);
    debug_control_fault(&c, "EVENT UART_ERROR\r\n");
    debug_control_update(&c, 100, false); assert(!c.pending && !running_motor && c.locked);
    c = fresh(0); stop_ok = false;
    debug_control_line(&c, "RUN 2 M1 800 0", 0); assert(c.locked && !c.pending);
    c = fresh(0); run_ok = false;
    debug_control_line(&c, "RUN 2 M1 800 0", 0);
    debug_control_update(&c, 100, false); assert(c.locked && !running_motor);

    c = fresh(UINT32_MAX - 150U);
    debug_control_line(&c, "RUN 2 M1 -800 200", UINT32_MAX - 150U);
    debug_control_update(&c, UINT32_MAX - 50U, false); assert(running_motor == 1);
    debug_control_update(&c, 149U, false); assert(running_motor == 0);
    printf("debug control: all tests passed (%u stop calls)\n", stops);
    return 0;
}
