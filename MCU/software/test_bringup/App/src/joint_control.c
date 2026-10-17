#include "joint_control.h"

#include <stdio.h>
#include <string.h>

#include "motor_limits.h"

/** @brief 12 位 ADC 满量程；读数为 0 或满量程视为电位器断线或短路。 */
#define ADC_RAW_FULL (4095)

/** @brief 标定与目标角度的绝对值上限，毫度。 */
#define ANGLE_LIMIT (360000)

/** @brief 两个标定点之间至少相差的 ADC 读数。 */
#define CAL_MIN_RAW_SPAN (100)

/** @brief 两个标定点之间至少相差的角度，毫度。 */
#define CAL_MIN_ANGLE_SPAN (1000)

/** @brief 到达容差的允许范围，毫度。 */
#define TOLERANCE_MIN (100U)
#define TOLERANCE_MAX (10000U)

/** @brief 单次运动超时的允许范围，单位毫秒。 */
#define MOVE_TIMEOUT_MIN_MS (500U)
#define MOVE_TIMEOUT_MAX_MS (60000U)

/** @brief 在容差内连续保持该时间才判定到达，单位毫秒。 */
#define REACHED_HOLD_MS (200U)

/** @brief 误差至少减小这么多才算有进展，毫度。 */
#define PROGRESS_STEP (500)

/** @brief 超过该时间没有进展判定为停滞，单位毫秒。 */
#define STALL_MS (2000U)

/** @brief 误差比初始值多出 2 倍容差加该余量即判定方向反了，毫度。 */
#define DIRECTION_MARGIN (1000U)

/** @brief 换向前停顿时间，单位毫秒。 */
#define REVERSE_SETTLE_MS (100U)

/** @brief 误差小于 5 倍容差加该余量时改为点动，毫度。 */
#define NEAR_MARGIN (5000U)

/** @brief 点动脉冲：驱动段与停顿段时长，单位毫秒。 */
#define PULSE_ON_MS (30U)
#define PULSE_OFF_MS (100U)

static int32_t magnitude(int32_t n)
{
    return n < 0 ? -n : n;
}
bool joint_cal_valid(const joint_cal_t *c)
{
    return c->raw_a > 0 && c->raw_a < ADC_RAW_FULL && c->raw_b > 0 &&
           c->raw_b < ADC_RAW_FULL &&
           magnitude((int32_t)c->raw_b - c->raw_a) >= CAL_MIN_RAW_SPAN &&
           c->angle_a >= -ANGLE_LIMIT && c->angle_a <= ANGLE_LIMIT &&
           c->angle_b >= -ANGLE_LIMIT && c->angle_b <= ANGLE_LIMIT &&
           c->angle_b - c->angle_a >= CAL_MIN_ANGLE_SPAN &&
           (c->direction == 1 || c->direction == -1) &&
           c->duty >= MOTOR_DUTY_MIN && c->duty <= MOTOR_DUTY_SMALL_MAX &&
           c->tolerance >= TOLERANCE_MIN && c->tolerance <= TOLERANCE_MAX &&
           (uint32_t)(c->angle_b - c->angle_a) > 2U * c->tolerance;
}
int32_t joint_angle(const joint_cal_t *c, uint16_t raw)
{
    return c->angle_a +
           (int32_t)(((int64_t)raw - c->raw_a) * (c->angle_b - c->angle_a) /
                     ((int32_t)c->raw_b - c->raw_a));
}
void joint_init(joint_control_t *j, joint_hw_t hw)
{
    memset(j, 0, sizeof(*j));
    j->hw = hw;
    j->active = -1;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        j->states[i].fault = "NONE";
    }
}
static bool output(joint_control_t *j, int duty)
{
    joint_state_t *s = &j->states[j->active];
    if (s->duty == duty)
    {
        return true;
    }
    if (!j->hw.drive((unsigned)j->active, duty))
    {
        return false;
    }
    s->duty = duty;
    return true;
}
static bool finish(joint_control_t *j, const char *result, joint_phase_t phase)
{
    unsigned index = (unsigned)j->active;
    bool ok = j->hw.drive(index, 0);
    joint_state_t *s = &j->states[index];
    s->duty = 0;
    if (!ok)
    {
        result = "HARDWARE";
        phase = JOINT_FAULT;
    }
    s->phase = phase;
    s->fault = phase == JOINT_FAULT ? result : "NONE";
    char line[80];
    (void)snprintf(line, sizeof(line), "JRESULT %u M%u %s\r\n", j->seq,
                   index + 1U, result);
    j->active = -1;
    j->hw.reply(line);
    return ok && phase != JOINT_FAULT;
}
bool joint_cancel(joint_control_t *j)
{
    if (j->active < 0)
    {
        return true;
    }
    return finish(j, "CANCELLED", JOINT_IDLE);
}
const char *joint_calibrate(joint_control_t *j, unsigned index, joint_cal_t c)
{
    if (index >= JOINT_COUNT || !joint_cal_valid(&c))
    {
        return "CAL_PARAM";
    }
    if (j->active >= 0)
    {
        return "BUSY";
    }
    joint_state_t *s = &j->states[index];
    s->cal = c;
    s->calibrated = true;
    s->phase = JOINT_IDLE;
    s->fault = "NONE";
    if (j->valid)
    {
        s->angle = joint_angle(&c, s->raw);
    }
    s->target = s->angle;
    return NULL;
}
const char *joint_move(joint_control_t *j, unsigned seq, unsigned index,
                       int32_t target, uint32_t timeout, uint32_t now)
{
    if (index >= JOINT_COUNT)
    {
        return "JOINT_ID";
    }
    joint_state_t *s = &j->states[index];
    if (!s->calibrated)
    {
        return "NOT_CALIBRATED";
    }
    if (!j->valid || (uint32_t)(now - j->sample_ms) > JOINT_SENSOR_STALE_MS)
    {
        return "SENSOR_STALE";
    }
    if (target < s->cal.angle_a || target > s->cal.angle_b ||
        timeout < MOVE_TIMEOUT_MIN_MS || timeout > MOVE_TIMEOUT_MAX_MS)
    {
        return "TARGET_RANGE";
    }
    if (s->raw == 0 || s->raw >= ADC_RAW_FULL ||
        s->angle < s->cal.angle_a - (int32_t)s->cal.tolerance ||
        s->angle > s->cal.angle_b + (int32_t)s->cal.tolerance)
    {
        return "POSITION_RANGE";
    }
    if (j->active >= 0)
    {
        return "BUSY";
    }
    j->active = (int)index;
    j->seq = seq;
    j->start_ms = now;
    j->progress_ms = now;
    j->timeout_ms = timeout;
    j->switch_ms = now;
    j->last_sign = 0;
    j->stable = false;
    j->initial_error = magnitude(target - s->angle);
    j->best_error = j->initial_error;
    j->pulse_on = false;
    j->near_mode = false;
    j->pulse_ms = now;
    s->target = target;
    s->phase = JOINT_SETTLE;
    s->fault = "NONE";
    return NULL;
}
void joint_feed(joint_control_t *j, const uint16_t *raw, bool valid,
                uint32_t now)
{
    j->valid = valid;
    if (!valid)
    {
        return;
    }
    j->sample_ms = now;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        j->states[i].raw = raw[i];
        if (j->states[i].calibrated)
        {
            j->states[i].angle = joint_angle(&j->states[i].cal, raw[i]);
        }
    }
}
bool joint_tick(joint_control_t *j, uint32_t now)
{
    if (j->active < 0)
    {
        return true;
    }
    joint_state_t *s = &j->states[j->active];
    if (!j->valid || (uint32_t)(now - j->sample_ms) > JOINT_SENSOR_STALE_MS ||
        s->raw == 0 || s->raw >= ADC_RAW_FULL)
    {
        return finish(j, "SENSOR", JOINT_FAULT);
    }
    if ((uint32_t)(now - j->start_ms) >= j->timeout_ms)
    {
        return finish(j, "TIMEOUT", JOINT_FAULT);
    }
    if (s->angle < s->cal.angle_a - (int32_t)s->cal.tolerance ||
        s->angle > s->cal.angle_b + (int32_t)s->cal.tolerance)
    {
        return finish(j, "LIMIT", JOINT_FAULT);
    }
    int32_t error = s->target - s->angle, distance = magnitude(error);
    if (distance <= (int32_t)s->cal.tolerance)
    {
        if (!output(j, 0))
        {
            return finish(j, "HARDWARE", JOINT_FAULT);
        }
        if (!j->stable)
        {
            j->stable = true;
            j->stable_ms = now;
        }
        if ((uint32_t)(now - j->stable_ms) >= REACHED_HOLD_MS)
        {
            return finish(j, "REACHED", JOINT_DONE);
        }
        return true;
    }
    j->stable = false;
    if (distance + PROGRESS_STEP < j->best_error)
    {
        j->best_error = distance;
        j->progress_ms = now;
    }
    if (distance >
        j->initial_error + (int32_t)(2U * s->cal.tolerance + DIRECTION_MARGIN))
    {
        return finish(j, "DIRECTION", JOINT_FAULT);
    }
    if ((uint32_t)(now - j->progress_ms) >= STALL_MS)
    {
        return finish(j, "STALL", JOINT_FAULT);
    }
    int sign = error > 0 ? 1 : -1;
    if (sign != j->last_sign)
    {
        if (!output(j, 0))
        {
            return finish(j, "HARDWARE", JOINT_FAULT);
        }
        j->last_sign = sign;
        j->switch_ms = now;
        s->phase = JOINT_SETTLE;
    }
    if ((uint32_t)(now - j->switch_ms) < REVERSE_SETTLE_MS)
    {
        return true;
    }
    s->phase = JOINT_MOVING;
    /* 接近目标时改为短脉冲点动（驱动 PULSE_ON_MS、停 PULSE_OFF_MS），
     * 因为低占空比电机根本转不动。 */
    bool near = distance <= (int32_t)(5U * s->cal.tolerance + NEAR_MARGIN);
    if (near && !j->near_mode)
    {
        j->pulse_on = false;
        j->pulse_ms = now;
    }
    j->near_mode = near;
    if (!near)
    {
        j->pulse_on = false;
    }
    int duty = sign * s->cal.direction * (int)s->cal.duty;
    if (near)
    {
        if (!j->pulse_on && (uint32_t)(now - j->pulse_ms) >= PULSE_OFF_MS)
        {
            j->pulse_on = true;
            j->pulse_ms = now;
        }
        else if (j->pulse_on && (uint32_t)(now - j->pulse_ms) >= PULSE_ON_MS)
        {
            j->pulse_on = false;
            j->pulse_ms = now;
        }
        if (!j->pulse_on)
        {
            duty = 0;
        }
    }
    if (!output(j, duty))
    {
        return finish(j, "HARDWARE", JOINT_FAULT);
    }
    return true;
}
const char *joint_phase_name(joint_phase_t phase)
{
    static const char *const names[JOINT_PHASE_COUNT] = {
        "IDLE", "SETTLE", "MOVE", "DONE", "FAULT"};
    return ((unsigned)phase < JOINT_PHASE_COUNT) ? names[phase] : "?";
}
