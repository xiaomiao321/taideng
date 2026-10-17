#include "joint_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int output_duty;
static bool hardware_ok;
static char reply[100];
static bool drive(unsigned index, int duty)
{
    assert(index < 6);
    output_duty = duty;
    return hardware_ok;
}
static void emit(const char *text)
{
    snprintf(reply, sizeof(reply), "%s", text);
}
static joint_cal_t normal(void)
{
    joint_cal_t c = {1000, 3000, 0, 180000, 1, 800, 2000};
    return c;
}
static joint_control_t fresh(joint_cal_t cal)
{
    joint_control_t j;
    joint_hw_t hw = {drive, emit};
    hardware_ok = true;
    output_duty = 0;
    reply[0] = 0;
    joint_init(&j, hw);
    assert(!joint_calibrate(&j, 0, cal));
    return j;
}
static void feed(joint_control_t *j, uint16_t raw, uint32_t now)
{
    uint16_t values[6] = {raw, 2000, 2000, 2000, 2000, 2000};
    joint_feed(j, values, true, now);
}
static void plant_test(bool reversed)
{
    joint_cal_t cal = normal();
    if (reversed)
    {
        cal.raw_a = 3000;
        cal.raw_b = 1000;
        cal.direction = -1;
    }
    joint_control_t j = fresh(cal);
    double raw = 2000;
    feed(&j, (uint16_t)raw, 0);
    assert(!joint_move(&j, 10, 0, 130000, 20000, 0));
    for (uint32_t now = 0; now < 20000 && j.active >= 0; now += 10)
    {
        raw += output_duty / 1000.0 * 5.0;
        feed(&j, (uint16_t)raw, now);
        assert(joint_tick(&j, now));
    }
    assert(j.active == -1 && j.states[0].phase == JOINT_DONE &&
           output_duty == 0);
    assert(j.states[0].angle >= 128000 && j.states[0].angle <= 132000);
    assert(strstr(reply, "REACHED"));
}
int main(void)
{
    joint_cal_t cal = normal();
    assert(joint_cal_valid(&cal));
    assert(joint_angle(&cal, 2000) == 90000);
    joint_cal_t reverse = cal;
    reverse.raw_a = 3000;
    reverse.raw_b = 1000;
    assert(joint_cal_valid(&reverse) && joint_angle(&reverse, 2000) == 90000);
    joint_cal_t bad = cal;
    bad.raw_b = 1050;
    assert(!joint_cal_valid(&bad));
    bad = cal;
    bad.angle_b = bad.angle_a;
    assert(!joint_cal_valid(&bad));
    bad = cal;
    bad.direction = 0;
    assert(!joint_cal_valid(&bad));
    bad = cal;
    bad.angle_a = INT32_MAX;
    assert(!joint_cal_valid(&bad));
    bad = cal;
    bad.duty = 499;
    assert(!joint_cal_valid(&bad));
    bad = cal;
    bad.duty = 500;
    assert(joint_cal_valid(&bad));
    bad = cal;
    bad.duty = 1000;
    assert(joint_cal_valid(&bad));
    bad = cal;
    bad.tolerance = 10001;
    assert(!joint_cal_valid(&bad));
    plant_test(false);
    plant_test(true);
    joint_control_t j = fresh(cal);
    assert(joint_move(&j, 1, 6, 90000, 10000, 0));
    assert(joint_move(&j, 1, 1, 90000, 10000, 0)); /* M2 not calibrated */
    assert(joint_move(&j, 1, 0, 90000, 10000, 0)); /* no feedback */
    feed(&j, 2000, 0);
    assert(joint_move(&j, 1, 0, 180001, 10000, 0));
    assert(joint_move(&j, 1, 0, 90000, 100, 0));
    assert(!joint_move(&j, 1, 0, 90000, 10000, 0));
    for (uint32_t t = 0; t <= 200; t += 10)
    {
        feed(&j, 2000, t);
        assert(joint_tick(&j, t));
        assert(output_duty == 0);
    }
    assert(j.states[0].phase == JOINT_DONE);
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 2, 0, 150000, 10000, 0));
    for (uint32_t t = 0; t < 2000; t += 10)
    {
        feed(&j, 2000, t);
        assert(joint_tick(&j, t));
    }
    feed(&j, 2000, 2000);
    assert(!joint_tick(&j, 2000));
    assert(!strcmp(j.states[0].fault, "STALL"));
    assert(output_duty == 0);
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 3, 0, 150000, 10000, 0));
    assert(!joint_tick(&j, 51));
    assert(!strcmp(j.states[0].fault, "SENSOR"));
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 4, 0, 150000, 10000, 0));
    joint_feed(&j, NULL, false, 10);
    assert(!joint_tick(&j, 10));
    assert(output_duty == 0);
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 5, 0, 150000, 10000, 0));
    feed(&j, 3100, 10);
    assert(!joint_tick(&j, 10));
    assert(!strcmp(j.states[0].fault, "LIMIT"));
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 6, 0, 150000, 10000, 0));
    feed(&j, 1900, 10);
    assert(!joint_tick(&j, 10));
    assert(!strcmp(j.states[0].fault, "DIRECTION"));
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 7, 0, 150000, 500, 0));
    feed(&j, 2000, 500);
    assert(!joint_tick(&j, 500));
    assert(!strcmp(j.states[0].fault, "TIMEOUT"));
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 8, 0, 150000, 10000, 0));
    assert(joint_cancel(&j) && output_duty == 0 && j.active == -1 &&
           strstr(reply, "CANCELLED"));
    j = fresh(cal);
    feed(&j, 2000, UINT32_MAX - 100);
    assert(!joint_move(&j, 9, 0, 90000, 10000, UINT32_MAX - 100));
    for (uint32_t t = 0; t <= 200; t += 10)
    {
        feed(&j, 2000, UINT32_MAX - 100 + t);
        assert(joint_tick(&j, UINT32_MAX - 100 + t));
    }
    assert(j.states[0].phase == JOINT_DONE);
    puts("joint control: calibration, inverse sensor, simulated plant, "
         "arrival, stale ADC, limits, direction, stall, timeout, cancellation "
         "and rollover passed");
    return 0;
}
