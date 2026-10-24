#include "joint_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int output_duty;
static bool hardware_ok;
static bool is_braking;
static unsigned brake_calls;
static char reply[100];
static bool drive(unsigned index, int duty)
{
    assert(index < 6);
    output_duty = duty;
    is_braking = false;
    return hardware_ok;
}
static bool brake_motor(unsigned index)
{
    assert(index < 6);
    output_duty = 0;
    is_braking = true;
    ++brake_calls;
    return hardware_ok;
}
static void emit(const char *text)
{
    snprintf(reply, sizeof(reply), "%s", text);
}
static joint_cal_t normal(void)
{
    joint_cal_t c = {1000, 3000, 0, 180000, 1, 800, 2000, 0, 180000};
    return c;
}
static joint_control_t fresh(joint_cal_t cal)
{
    joint_control_t j;
    joint_hw_t hw = {drive, emit, brake_motor};
    hardware_ok = true;
    output_duty = 0;
    is_braking = false;
    brake_calls = 0;
    reply[0] = 0;
    joint_init(&j, hw);
    assert(!joint_calibrate(&j, 0, cal));
    return j;
}
static void feed(joint_control_t *j, uint16_t raw, uint32_t now)
{
    uint16_t values[6] = {raw, 2000, 2000, 2000, 2000, 2000};
    joint_feed(j, values, NULL, true, now);
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
/**
 * 带惯性的电机模型：驱动时速度趋向占空比对应的稳态值，滑行时速度缓慢衰减，
 * 制动时迅速衰减。返回最大过冲（毫度，越过目标的量）。
 */
static int32_t inertia_test(joint_tune_t tune, int32_t target)
{
    joint_control_t j = fresh(normal());
    assert(!joint_set_tune(&j, 0, tune));
    double raw = 2000, speed = 0; /* 读数/10ms */
    feed(&j, (uint16_t)raw, 0);
    int32_t start = j.states[0].angle, overshoot = 0;
    assert(!joint_move(&j, 20, 0, target, 20000, 0));
    for (uint32_t now = 0; now < 20000 && j.active >= 0; now += 10)
    {
        double steady = output_duty / 1000.0 * 8.0;
        speed += is_braking ? -speed * 0.8
                 : output_duty ? (steady - speed) * 0.2
                               : -speed * 0.05;
        raw += speed;
        feed(&j, (uint16_t)raw, now);
        int32_t past = (target > start) ? j.states[0].angle - target
                                        : target - j.states[0].angle;
        overshoot = past > overshoot ? past : overshoot;
        assert(joint_tick(&j, now));
    }
    assert(j.active == -1 && j.states[0].phase == JOINT_DONE);
    assert(strstr(reply, "REACHED"));
    return overshoot;
}
static void tune_test(void)
{
    joint_control_t j = fresh(normal());
    joint_tune_t def = j.states[0].tune;
    assert(def.mode == JOINT_MODE_BANG);
    joint_tune_t p = {JOINT_MODE_P, 30, 300, 50};
    assert(!joint_set_tune(&j, 1, p)); /* 不要求已标定 */
    joint_tune_t bad = p;
    bad.mode = JOINT_MODE_COUNT;
    assert(!strcmp(joint_set_tune(&j, 0, bad), "TUNE_PARAM"));
    bad = p;
    bad.kp = JOINT_KP_MAX + 1U;
    assert(joint_set_tune(&j, 0, bad));
    bad = p;
    bad.duty_min = 99;
    assert(joint_set_tune(&j, 0, bad));
    bad = p;
    bad.brake_ms = JOINT_BRAKE_MAX_MS + 1U;
    assert(joint_set_tune(&j, 0, bad));
    assert(!strcmp(joint_set_tune(&j, 6, p), "JOINT_ID"));

    /* 比例式占空比：远处饱和到 cal.duty，近处为起转占空比加比例项。 */
    assert(!joint_set_tune(&j, 0, p));
    feed(&j, 2000, 0); /* 90° */
    assert(!joint_move(&j, 21, 0, 150000, 10000, 0));
    assert(!strcmp(joint_set_tune(&j, 0, p), "BUSY"));
    assert(joint_tick(&j, 0) && output_duty == 0); /* 换向停顿 */
    feed(&j, 2000, 100);
    assert(joint_tick(&j, 100) && output_duty == 800);
    feed(&j, 2611, 110); /* 约 145°，误差约 5° */
    assert(joint_tick(&j, 110));
    assert(output_duty >= 300 + 140 && output_duty <= 300 + 160);
    /* 进入容差：先制动，制动时长到后松开为滑行，保持后判定到达。 */
    feed(&j, 2667, 120); /* 约 150° */
    assert(joint_tick(&j, 120) && is_braking && brake_calls == 1);
    feed(&j, 2667, 160);
    assert(joint_tick(&j, 160) && is_braking);
    feed(&j, 2667, 170);
    assert(joint_tick(&j, 170) && !is_braking && output_duty == 0);
    feed(&j, 2667, 320);
    assert(joint_tick(&j, 320));
    assert(j.active == -1 && j.states[0].phase == JOINT_DONE);

    /* 过冲换向：比例式先制动再反向。 */
    j = fresh(normal());
    assert(!joint_set_tune(&j, 0, p));
    feed(&j, 2000, 0);
    assert(!joint_move(&j, 22, 0, 100000, 10000, 0));
    assert(joint_tick(&j, 0));
    feed(&j, 2000, 100);
    assert(joint_tick(&j, 100) && output_duty > 0);
    feed(&j, 2200, 110); /* 约 108°，越过目标 */
    assert(joint_tick(&j, 110) && is_braking && output_duty == 0);
    feed(&j, 2200, 210);
    assert(joint_tick(&j, 210) && output_duty < 0 && !is_braking);
    assert(joint_cancel(&j));

    /* 没有制动回调时退化为滑行。 */
    joint_control_t plain;
    joint_hw_t hw = {drive, emit, NULL};
    joint_init(&plain, hw);
    assert(!joint_calibrate(&plain, 0, normal()));
    assert(!joint_set_tune(&plain, 0, p));
    brake_calls = 0;
    feed(&plain, 2000, 0);
    assert(!joint_move(&plain, 23, 0, 90500, 10000, 0));
    assert(joint_tick(&plain, 0) && brake_calls == 0 && output_duty == 0);
    assert(joint_cancel(&plain));

    /* 带惯性的模型：两种方式都能到位，比例式加制动的过冲明显更小。 */
    int32_t bang = inertia_test(def, 150000);
    int32_t prop = inertia_test(p, 150000);
    assert(prop < bang);
    (void)inertia_test(p, 30000);
    printf("inertia overshoot: bang %ld mdeg, proportional %ld mdeg\n",
           (long)bang, (long)prop);
}
static void filter_test(void)
{
    uint16_t samples[16][JOINT_COUNT];
    for (unsigned n = 0; n < 16; ++n)
    {
        for (unsigned ch = 0; ch < JOINT_COUNT; ++ch)
        {
            samples[n][ch] = (uint16_t)(2000 + ch + (n % 2));
        }
    }
    /* 四个离群点被去掉，不影响结果。 */
    samples[3][0] = 4095;
    samples[7][0] = 0;
    samples[9][0] = 3000;
    samples[12][0] = 100;
    uint16_t out[JOINT_COUNT], spread[JOINT_COUNT];
    joint_filter((const uint16_t(*)[JOINT_COUNT])samples, 16, out, spread);
    assert(out[0] == 2001 || out[0] == 2000);
    assert(spread[0] == 4095);
    for (unsigned ch = 1; ch < JOINT_COUNT; ++ch)
    {
        assert(out[ch] == 2000 + ch + 1 || out[ch] == 2000 + ch);
        assert(spread[ch] == 1);
    }
    joint_filter((const uint16_t(*)[JOINT_COUNT])samples, 1, out, spread);
    assert(out[1] == 2001 && spread[1] == 0);

    joint_control_t j = fresh(normal());
    uint16_t raw[JOINT_COUNT] = {2000, 2000, 2000, 2000, 2000, 2000};
    uint16_t noise[JOINT_COUNT] = {12, 0, 0, 0, 0, 0};
    joint_feed(&j, raw, noise, true, 0);
    assert(j.states[0].spread == 12 && j.states[1].spread == 0);
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
    bad.duty = 99;
    assert(!joint_cal_valid(&bad));
    bad = cal;
    bad.duty = 100;
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
    joint_feed(&j, NULL, NULL, false, 10);
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

    /* 标定导出/导入：两个关节（含负角度、反向）往返后参数逐项相同。 */
    joint_control_t source = fresh(cal);
    joint_cal_t other = {3900, 200,  -45000, 30000, -1,
                         650,  1500, -40000, 25000};
    assert(!joint_calibrate(&source, 4, other));
    uint8_t blob[JOINT_CAL_BLOB_SIZE];
    joint_cal_export(&source, blob);
    joint_control_t restored = fresh(normal());
    restored.states[0].calibrated = false;
    assert(joint_cal_import(&restored, blob) == 2U);
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        assert(restored.states[i].calibrated == source.states[i].calibrated);
        if (source.states[i].calibrated)
        {
            assert(!memcmp(&restored.states[i].cal, &source.states[i].cal,
                           sizeof(joint_cal_t)));
        }
    }
    /* 版本不符：什么都不加载。 */
    joint_control_t untouched = fresh(normal());
    untouched.states[0].calibrated = false;
    blob[0] = (uint8_t)(JOINT_CAL_BLOB_VERSION + 1U);
    assert(joint_cal_import(&untouched, blob) == 0U);
    assert(!untouched.states[0].calibrated && !untouched.states[4].calibrated);
    /* 数据损坏成非法参数（占空比 0）：该项被跳过，其余照常恢复。 */
    blob[0] = (uint8_t)JOINT_CAL_BLOB_VERSION;
    blob[4U + 4U * JOINT_CAL_BLOB_ENTRY_SIZE + 2U] = 0U;
    blob[4U + 4U * JOINT_CAL_BLOB_ENTRY_SIZE + 3U] = 0U;
    assert(joint_cal_import(&untouched, blob) == 1U);
    assert(untouched.states[0].calibrated && !untouched.states[4].calibrated);

    /* 理论标定：47k 电位器，角度增大读数减小，参考点 90° 读 2048。
     * 每度 4095*47000/49000/270 ≈ 14.547 个读数，10° 与 170° 距参考点
     * 各 80°，即 ±1163.75 个读数。 */
    joint_nominal_t nominal = {
        .raw_ref = 2048,
        .angle_ref = 90000,
        .sensor_direction = -1,
        .direction = 1,
        .duty = 800,
        .tolerance = 2000,
        .limit_min = 10000,
        .limit_max = 170000,
        .pot_ohms = 47000,
    };
    joint_cal_t theory;
    assert(!joint_cal_from_nominal(&nominal, &theory));
    assert(theory.raw_a == 3212 && theory.raw_b == 884);
    assert(theory.angle_a == 10000 && theory.angle_b == 170000);
    assert(theory.limit_min == 10000 && theory.limit_max == 170000);
    int32_t at_ref = joint_angle(&theory, 2048);
    assert(at_ref > 89900 && at_ref < 90100);
    /* 阻值填 0 采用 45k 标称值，仍然成立。 */
    joint_nominal_t defaulted = nominal;
    defaulted.pot_ohms = 0;
    assert(!joint_cal_from_nominal(&defaulted, &theory));
    /* 允许范围超出电位器电气行程、方向或阻值无效：拒绝。 */
    joint_nominal_t wrong = nominal;
    wrong.limit_max = 260000;
    assert(joint_cal_from_nominal(&wrong, &theory));
    wrong = nominal;
    wrong.sensor_direction = 0;
    assert(joint_cal_from_nominal(&wrong, &theory));
    wrong = nominal;
    wrong.pot_ohms = 500;
    assert(joint_cal_from_nominal(&wrong, &theory));
    wrong = nominal;
    wrong.raw_ref = 20; /* 低于下端固定电阻对应的读数，不可能出现 */
    assert(joint_cal_from_nominal(&wrong, &theory));

    /* 允许范围与标定点分开：收窄范围后，范围外的目标被拒绝。 */
    j = fresh(cal);
    feed(&j, 2000, 0);
    assert(!joint_set_limits(&j, 0, 20000, 160000));
    assert(j.states[0].cal.angle_a == 0 && j.states[0].cal.angle_b == 180000);
    assert(joint_move(&j, 1, 0, 170000, 10000, 0));
    assert(!joint_move(&j, 1, 0, 150000, 10000, 0));
    assert(!strcmp(joint_set_limits(&j, 0, 0, 100000), "BUSY"));
    assert(joint_cancel(&j));
    assert(!strcmp(joint_set_limits(&j, 1, 0, 100000), "NOT_CALIBRATED"));
    /* 读数 1 对应约 -89.9°：超出可测区间的范围被拒绝。 */
    assert(!strcmp(joint_set_limits(&j, 0, -100000, 100000), "LIMIT_PARAM"));
    assert(!strcmp(joint_set_limits(&j, 0, 50000, 50000), "LIMIT_PARAM"));
    assert(j.states[0].cal.limit_min == 20000);

    tune_test();
    filter_test();
    puts("joint control: calibration, inverse sensor, simulated plant, "
         "arrival, stale ADC, limits, direction, stall, timeout, cancellation, "
         "rollover, calibration export/import, nominal calibration, limits, "
         "proportional mode with braking and ADC filter passed");
    return 0;
}
