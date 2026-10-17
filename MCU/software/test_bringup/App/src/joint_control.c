#include "joint_control.h"
#include "motor_limits.h"
#include <stdio.h>
#include <string.h>

static int32_t magnitude(int32_t n)
{
    return n < 0 ? -n : n;
}
bool joint_cal_valid(const joint_cal_t *c)
{
    return c->raw_a > 0 && c->raw_a < 4095 && c->raw_b > 0 && c->raw_b < 4095 &&
           magnitude((int32_t)c->raw_b - c->raw_a) >= 100 &&
           c->angle_a >= -360000 && c->angle_a <= 360000 &&
           c->angle_b >= -360000 && c->angle_b <= 360000 &&
           c->angle_b - c->angle_a >= 1000 &&
           (c->direction == 1 || c->direction == -1) &&
           c->duty >= MOTOR_DUTY_MIN && c->duty <= MOTOR_DUTY_SMALL_MAX &&
           c->tolerance >= 100 && c->tolerance <= 10000 &&
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
    if (!j->valid || (uint32_t)(now - j->sample_ms) > 50U)
    {
        return "SENSOR_STALE";
    }
    if (target < s->cal.angle_a || target > s->cal.angle_b || timeout < 500U ||
        timeout > 60000U)
    {
        return "TARGET_RANGE";
    }
    if (s->raw == 0 || s->raw >= 4095 ||
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
    if (!j->valid || (uint32_t)(now - j->sample_ms) > 50U || s->raw == 0 ||
        s->raw >= 4095)
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
        if ((uint32_t)(now - j->stable_ms) >= 200U)
        {
            return finish(j, "REACHED", JOINT_DONE);
        }
        return true;
    }
    j->stable = false;
    if (distance + 500 < j->best_error)
    {
        j->best_error = distance;
        j->progress_ms = now;
    }
    if (distance > j->initial_error + (int32_t)(2U * s->cal.tolerance + 1000U))
    {
        return finish(j, "DIRECTION", JOINT_FAULT);
    }
    if ((uint32_t)(now - j->progress_ms) >= 2000U)
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
    if ((uint32_t)(now - j->switch_ms) < 100U)
    {
        return true;
    }
    s->phase = JOINT_MOVING;
    /* Near the target, use 30ms drive / 100ms coast instead of an unusable low PWM. */
    bool near = distance <= (int32_t)(5U * s->cal.tolerance + 5000U);
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
        if (!j->pulse_on && (uint32_t)(now - j->pulse_ms) >= 100U)
        {
            j->pulse_on = true;
            j->pulse_ms = now;
        }
        else if (j->pulse_on && (uint32_t)(now - j->pulse_ms) >= 30U)
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
    static const char *const names[] = {"IDLE", "SETTLE", "MOVE", "DONE",
                                        "FAULT"};
    return names[phase];
}
