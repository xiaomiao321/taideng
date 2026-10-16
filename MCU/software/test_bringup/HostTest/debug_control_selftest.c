#include "debug_control.h"
#include "motor_limits.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned running_motor, stops;
static int running_duty;
static bool stop_ok = true, run_ok = true;
static int outputs[8];
static unsigned manual_calls;
static char reply[128];
static unsigned test_calls, confirm_calls;
static unsigned joint_calls;
static const char *hw_joint(const char *op, unsigned seq, unsigned id, const int32_t *v, unsigned count, uint32_t now)
{
    (void)seq; (void)now; assert(id == 0);
    if (!strcmp(op, "CAL")) { assert(count == 7 && v[0] == 600 && v[1] == -30000 && v[6] == 2000); }
    else if (!strcmp(op, "JMOVE")) { assert(count == 2 && v[0] == 45000 && v[1] == 10000); }
    else { assert(!strcmp(op, "JGET") && count == 0); }
    ++joint_calls; return NULL;
}
static const char *hw_test(unsigned seq, const char *name, int value, uint32_t duration)
{
    assert(running_motor == 0);
    assert(seq == 20 && !strcmp(name, "BUZZER") && value == 2700 && duration == 500);
    ++test_calls; return NULL;
}
static const char *hw_confirm(bool pass) { assert(pass); ++confirm_calls; return NULL; }
static bool hw_stop(void) { ++stops; running_motor = 0; running_duty = 0; memset(outputs, 0, sizeof(outputs)); return stop_ok; }
static bool hw_run(unsigned motor, int duty) { running_motor = motor; running_duty = duty; outputs[motor-1] = duty; return run_ok; }
static bool hw_manual(void) { ++manual_calls; return true; }
static void hw_reply(const char *s) { snprintf(reply, sizeof(reply), "%s", s); }
static debug_control_t fresh(uint32_t now)
{
    debug_control_t c;
    debug_hw_t hw = {.stop = hw_stop, .run = hw_run, .reply = hw_reply};
    stop_ok = true; run_ok = true;
    debug_control_init(&c, hw);
    debug_control_line(&c, "PING 1", now);
    return c;
}
int main(void)
{
    debug_control_t multi = fresh(0); multi.hw.manual = hw_manual;
    debug_control_line(&multi, "MSET 10 255 500 -600 700 -800 900 -1000 800 -800 500 600 700 800 900 1000 0 0", 0);
    assert(multi.multi_pending == 255 && manual_calls == 1);
    debug_control_update(&multi, 99, false); assert(!outputs[0] && !outputs[7]);
    debug_control_update(&multi, 100, false);
    assert(outputs[0] == 500 && outputs[1] == -600 && outputs[5] == -1000 && outputs[7] == -800);
    debug_control_line(&multi, "MSET 11 2 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0", 200);
    assert(outputs[0] == 500 && outputs[1] == 0 && outputs[7] == -800);
    unsigned before = manual_calls;
    debug_control_line(&multi, "MSET 12 1 499 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0", 210);
    assert(strstr(reply, "DUTY_RANGE") && manual_calls == before && outputs[0] == 500);
    debug_control_line(&multi, "MSET 12 1 -800 0 0 0 0 0 0 0 200 0 0 0 0 0 0 0", 220);
    assert(!outputs[0] && outputs[2] == 700);
    debug_control_update(&multi, 319, false); assert(!outputs[0]);
    debug_control_update(&multi, 320, false); assert(outputs[0] == -800);
    debug_control_update(&multi, 520, false); assert(!outputs[0] && outputs[2] == 700);
    debug_control_update(&multi, 800, false); assert(!outputs[2] && outputs[3] == -800 && outputs[7] == -800);
    debug_control_update(&multi, 1000, false); assert(multi.locked && !outputs[7] && !multi.multi_pending);
    debug_control_line(&multi, "MOFF 13", 1001); assert(strstr(reply, "ACK") && multi.locked);
    multi = fresh(UINT32_MAX-150); multi.hw.manual = hw_manual;
    debug_control_line(&multi, "MSET 14 128 0 0 0 0 0 0 0 -800 0 0 0 0 0 0 0 200", UINT32_MAX-150);
    debug_control_update(&multi, UINT32_MAX-50, false); assert(outputs[7] == -800);
    debug_control_update(&multi, 149, false); assert(!outputs[7]);
    multi = fresh(0); multi.hw.manual = hw_manual;
    debug_control_line(&multi, "MSET 15 255 800 800 800 800 800 800 800 800 0 0 0 0 0 0 0 0", 0);
    debug_control_update(&multi, 100, false); assert(outputs[0] && outputs[7]);
    debug_control_update(&multi, 110, true); assert(!outputs[0] && !outputs[7] && multi.locked);
    multi = fresh(0); multi.hw.manual = hw_manual;
    debug_control_line(&multi, "MSET 15 255 800 800 800 800 800 800 800 800 0 0 0 0 0 0 0 0", 0);
    run_ok = false;
    debug_control_update(&multi, 100, false); assert(multi.locked && !outputs[0] && !outputs[7]);
    for (unsigned motor = 1; motor <= 8; ++motor) {
        int max = motor <= 6 ? 1000 : 800;
        assert(motor_duty_valid(motor, 0));
        assert(motor_duty_valid(motor, 500) && motor_duty_valid(motor, -500));
        assert(motor_duty_valid(motor, max) && motor_duty_valid(motor, -max));
        assert(!motor_duty_valid(motor, 499) && !motor_duty_valid(motor, -499));
        assert(!motor_duty_valid(motor, max+1) && !motor_duty_valid(motor, -max-1));
    }
    debug_control_t c = fresh(0);
    debug_control_line(&c, "RUN 2 M1 800 500", 0);
    assert(c.pending && running_motor == 0);
    debug_control_update(&c, 99, false); assert(running_motor == 0);
    debug_control_update(&c, 100, false); assert(running_motor == 1 && running_duty == 800);
    debug_control_update(&c, 599, false); assert(running_motor == 1);
    debug_control_update(&c, 600, false); assert(running_motor == 0 && !c.locked);

    c = fresh(100);
    debug_control_line(&c, "RUN 2 M8 -800 0", 100);
    debug_control_update(&c, 200, false); assert(running_motor == 8 && running_duty == -800);
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
        "RUN 2 M1 +800 500", "RUN 2 M1 499 500", "RUN 2 M7 801 500", "RUN 2 M8 -1000 500", "PING 3 extra", "RUN"};
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
    c = fresh(0); c.hw.test = hw_test; c.hw.confirm = hw_confirm;
    debug_control_line(&c, "INFO 10", 1); assert(strstr(reply, "CAPS BOARD_TEST 2"));
    debug_control_line(&c, "RUN 2 M1 800 0", 2);
    debug_control_update(&c, 102, false); assert(running_motor == 1);
    debug_control_line(&c, "TEST 20 BUZZER 2700 500", 103);
    assert(test_calls == 1 && !c.pending && running_motor == 0);
    assert(strstr(reply, "ACK 20 TEST"));
    debug_control_line(&c, "CONFIRM 21 PASS", 104); assert(confirm_calls == 1);
    c.locked = true;
    debug_control_line(&c, "TEST 20 BUZZER 2700 500", 105); assert(test_calls == 1 && strstr(reply, "LOCKED"));
    debug_control_line(&c, "CONFIRM 21 PASS", 106); assert(confirm_calls == 1 && strstr(reply, "LOCKED"));
    c.locked = false; c.home = true;
    debug_control_line(&c, "TEST 20 BUZZER 2700 500", 107); assert(test_calls == 1 && strstr(reply, "LOCKED"));
    c.home = false;
    debug_control_line(&c, "TEST 20 BUZZER 2700 500", 1000); assert(test_calls == 1 && strstr(reply, "NO_HEARTBEAT"));
    debug_control_line(&c, "PING 22", 1100);
    debug_control_line(&c, "TEST 20 BUZZER 2700 0", 1101); assert(test_calls == 1 && strstr(reply, "PARAM"));
    debug_control_line(&c, "CONFIRM 21 OTHER", 1102); assert(confirm_calls == 1 && strstr(reply, "PARAM"));
    c = fresh(0); c.hw.joint = hw_joint;
    debug_control_line(&c, "INFO 30", 0); assert(strstr(reply, "CAPS JOINT 1"));
    debug_control_line(&c, "CAL 31 M1 600 -30000 3400 90000 1 800 2000", 1);
    assert(joint_calls == 1 && strstr(reply, "ACK 31 CAL"));
    debug_control_line(&c, "JMOVE 32 M1 45000 10000", 2);
    assert(joint_calls == 2 && strstr(reply, "ACK 32 JMOVE"));
    c.locked = true;
    debug_control_line(&c, "JMOVE 33 M1 45000 10000", 3);
    assert(joint_calls == 2 && strstr(reply, "LOCKED"));
    c.linked = false;
    debug_control_line(&c, "JGET 34 M1", 4); assert(joint_calls == 3);
    c.locked = false;
    debug_control_line(&c, "JMOVE 35 M1 45000 10000", 5);
    assert(joint_calls == 3 && strstr(reply, "NO_HEARTBEAT"));
    debug_control_line(&c, "JGET 36 M7", 6);
    assert(joint_calls == 3 && strstr(reply, "JOINT_PARAM"));
    debug_control_line(&c, "CAL 37 M1 600 -30000 3400 90000 1 800 2000 extra", 7);
    assert(joint_calls == 3 && strstr(reply, "JOINT_PARAM"));
    printf("debug control: motor, board and joint protocol tests passed (%u stop calls)\n", stops);
    return 0;
}
