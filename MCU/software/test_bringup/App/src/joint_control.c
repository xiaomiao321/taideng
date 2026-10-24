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

/** @brief 上电默认的控制参数：开关式；切到比例式时的初值。 */
#define DEFAULT_KP (30U)
#define DEFAULT_DUTY_MIN (500U)
#define DEFAULT_BRAKE_MS (50U)

_Static_assert(JOINT_BRAKE_MAX_MS <= REACHED_HOLD_MS,
               "brake must end before the reached hold expires");

static int32_t magnitude(int32_t n)
{
    return n < 0 ? -n : n;
}
int32_t joint_angle(const joint_cal_t *c, uint16_t raw)
{
    return c->angle_a +
           (int32_t)(((int64_t)raw - c->raw_a) * (c->angle_b - c->angle_a) /
                     ((int32_t)c->raw_b - c->raw_a));
}

/** @brief 换算关系是否合法（两点、方向、占空比、容差），不检查允许范围。 */
static bool is_mapping_valid(const joint_cal_t *c)
{
    return c->raw_a > 0 && c->raw_a < ADC_RAW_FULL && c->raw_b > 0 &&
           c->raw_b < ADC_RAW_FULL &&
           magnitude((int32_t)c->raw_b - c->raw_a) >= CAL_MIN_RAW_SPAN &&
           c->angle_a >= -ANGLE_LIMIT && c->angle_a <= ANGLE_LIMIT &&
           c->angle_b >= -ANGLE_LIMIT && c->angle_b <= ANGLE_LIMIT &&
           c->angle_b - c->angle_a >= CAL_MIN_ANGLE_SPAN &&
           (c->direction == 1 || c->direction == -1) &&
           c->duty >= MOTOR_DUTY_MIN && c->duty <= MOTOR_DUTY_SMALL_MAX &&
           c->tolerance >= TOLERANCE_MIN && c->tolerance <= TOLERANCE_MAX;
}

/**
 * @brief 允许范围是否合法：不越界、宽于两倍容差、两端都在电位器可测区间内
 *        （对应读数 1～ADC_RAW_FULL-1，读数 0 或满量程会被判为传感器故障）。
 */
static bool are_limits_valid(const joint_cal_t *c, int32_t limit_min,
                             int32_t limit_max)
{
    if (limit_min < -ANGLE_LIMIT || limit_max > ANGLE_LIMIT ||
        limit_min >= limit_max ||
        (uint32_t)(limit_max - limit_min) <= 2U * c->tolerance)
    {
        return false;
    }
    int32_t edge_low = joint_angle(c, 1U);
    int32_t edge_high = joint_angle(c, ADC_RAW_FULL - 1U);
    if (edge_low > edge_high)
    {
        int32_t swap = edge_low;
        edge_low = edge_high;
        edge_high = swap;
    }
    return limit_min >= edge_low && limit_max <= edge_high;
}

bool joint_cal_valid(const joint_cal_t *c)
{
    return is_mapping_valid(c) &&
           are_limits_valid(c, c->limit_min, c->limit_max);
}

/** @brief 带符号整数除法，四舍五入到最近整数。 */
static int64_t divide_round(int64_t numerator, int64_t denominator)
{
    int64_t half = denominator / 2;
    return (numerator >= 0) ? (numerator + half) / denominator
                            : (numerator - half) / denominator;
}

const char *joint_cal_from_nominal(const joint_nominal_t *n, joint_cal_t *out)
{
    uint32_t ohms = (n->pot_ohms == 0U) ? JOINT_POT_DEFAULT_OHMS : n->pot_ohms;
    if (ohms < JOINT_POT_MIN_OHMS || ohms > JOINT_POT_MAX_OHMS ||
        (n->sensor_direction != 1 && n->sensor_direction != -1) ||
        n->limit_min < -ANGLE_LIMIT || n->limit_max > ANGLE_LIMIT ||
        n->angle_ref < -ANGLE_LIMIT || n->angle_ref > ANGLE_LIMIT)
    {
        return "CAL_PARAM";
    }

    /* 分压：读数 = 满量程 × (端电阻 + 抽头以下的电位器电阻) / 总电阻。
     * 电位器走完 JOINT_POT_TRAVEL_MDEG 时读数变化 满量程 × R / 总电阻。 */
    int64_t total = (int64_t)ohms + 2 * (int64_t)JOINT_POT_END_OHMS;
    int64_t end_low =
        divide_round((int64_t)ADC_RAW_FULL * JOINT_POT_END_OHMS, total);
    int64_t end_high = divide_round(
        (int64_t)ADC_RAW_FULL * ((int64_t)ohms + JOINT_POT_END_OHMS), total);
    int64_t denominator = total * JOINT_POT_TRAVEL_MDEG;

    int64_t raw_at[2];
    const int32_t angles[2] = {n->limit_min, n->limit_max};
    for (unsigned i = 0; i < 2U; ++i)
    {
        int64_t offset = divide_round((int64_t)n->sensor_direction *
                                          ((int64_t)angles[i] - n->angle_ref) *
                                          ADC_RAW_FULL * (int64_t)ohms,
                                      denominator);
        raw_at[i] = (int64_t)n->raw_ref + offset;
    }
    /* 参考点和允许范围两端都必须落在电位器电气行程内。 */
    if ((int64_t)n->raw_ref < end_low || (int64_t)n->raw_ref > end_high ||
        raw_at[0] < end_low || raw_at[0] > end_high || raw_at[1] < end_low ||
        raw_at[1] > end_high)
    {
        return "CAL_PARAM";
    }

    joint_cal_t cal = {
        .raw_a = (uint16_t)raw_at[0],
        .raw_b = (uint16_t)raw_at[1],
        .angle_a = n->limit_min,
        .angle_b = n->limit_max,
        .direction = n->direction,
        .duty = n->duty,
        .tolerance = n->tolerance,
        .limit_min = n->limit_min,
        .limit_max = n->limit_max,
    };
    if (!joint_cal_valid(&cal))
    {
        return "CAL_PARAM";
    }
    *out = cal;
    return NULL;
}
void joint_init(joint_control_t *j, joint_hw_t hw)
{
    memset(j, 0, sizeof(*j));
    j->hw = hw;
    j->active = -1;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        j->states[i].fault = "NONE";
        j->states[i].tune = (joint_tune_t){JOINT_MODE_BANG, DEFAULT_KP,
                                           DEFAULT_DUTY_MIN, DEFAULT_BRAKE_MS};
    }
}
static bool output(joint_control_t *j, int duty)
{
    joint_state_t *s = &j->states[j->active];
    if (s->duty == duty && !j->braking)
    {
        return true;
    }
    if (!j->hw.drive((unsigned)j->active, duty))
    {
        return false;
    }
    s->duty = duty;
    j->braking = false;
    return true;
}
/** @brief 开始短路制动；没有制动回调或制动时长为 0 时改为滑行。 */
static bool brake(joint_control_t *j, uint32_t now)
{
    joint_state_t *s = &j->states[j->active];
    if (!j->hw.brake || s->tune.brake_ms == 0U)
    {
        return output(j, 0);
    }
    if (j->braking)
    {
        return true;
    }
    if (!j->hw.brake((unsigned)j->active))
    {
        return false;
    }
    s->duty = 0;
    j->braking = true;
    j->brake_ms = now;
    return true;
}
/** @brief 制动时间到就松开为滑行。 */
static bool release_brake(joint_control_t *j, uint32_t now)
{
    const joint_state_t *s = &j->states[j->active];
    if (j->braking && (uint32_t)(now - j->brake_ms) >= s->tune.brake_ms)
    {
        return output(j, 0);
    }
    return true;
}
static bool finish(joint_control_t *j, const char *result, joint_phase_t phase)
{
    unsigned index = (unsigned)j->active;
    bool ok = j->hw.drive(index, 0);
    joint_state_t *s = &j->states[index];
    s->duty = 0;
    j->braking = false;
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
const char *joint_set_limits(joint_control_t *j, unsigned index,
                             int32_t limit_min, int32_t limit_max)
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
    if (j->active >= 0)
    {
        return "BUSY";
    }
    if (!are_limits_valid(&s->cal, limit_min, limit_max))
    {
        return "LIMIT_PARAM";
    }
    s->cal.limit_min = limit_min;
    s->cal.limit_max = limit_max;
    return NULL;
}

const char *joint_set_tune(joint_control_t *j, unsigned index,
                           joint_tune_t tune)
{
    if (index >= JOINT_COUNT)
    {
        return "JOINT_ID";
    }
    if ((unsigned)tune.mode >= JOINT_MODE_COUNT || tune.kp > JOINT_KP_MAX ||
        tune.brake_ms > JOINT_BRAKE_MAX_MS ||
        !motor_duty_valid(index, (int)tune.duty_min))
    {
        return "TUNE_PARAM";
    }
    if (j->active >= 0)
    {
        return "BUSY";
    }
    j->states[index].tune = tune;
    return NULL;
}
void joint_filter(const uint16_t (*samples)[JOINT_COUNT], unsigned count,
                  uint16_t out[JOINT_COUNT], uint16_t spread[JOINT_COUNT])
{
    for (unsigned ch = 0; ch < JOINT_COUNT; ++ch)
    {
        uint16_t v[JOINT_FILTER_MAX_SAMPLES];
        unsigned n = 0;
        for (; n < count && n < JOINT_FILTER_MAX_SAMPLES; ++n)
        {
            /* 插入排序：样本少，够用。 */
            uint16_t x = samples[n][ch];
            unsigned k = n;
            for (; k > 0 && v[k - 1] > x; --k)
            {
                v[k] = v[k - 1];
            }
            v[k] = x;
        }
        if (n == 0U)
        {
            out[ch] = 0U;
            spread[ch] = 0U;
            continue;
        }
        unsigned trim = n / 4U;
        uint32_t sum = 0U;
        for (unsigned k = trim; k < n - trim; ++k)
        {
            sum += v[k];
        }
        unsigned kept = n - 2U * trim;
        out[ch] = (uint16_t)((sum + kept / 2U) / kept);
        spread[ch] = (uint16_t)(v[n - 1U] - v[0]);
    }
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
    if (target < s->cal.limit_min || target > s->cal.limit_max ||
        timeout < MOVE_TIMEOUT_MIN_MS || timeout > MOVE_TIMEOUT_MAX_MS)
    {
        return "TARGET_RANGE";
    }
    if (s->raw == 0 || s->raw >= ADC_RAW_FULL ||
        s->angle < s->cal.limit_min - (int32_t)s->cal.tolerance ||
        s->angle > s->cal.limit_max + (int32_t)s->cal.tolerance)
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
    j->braking = false;
    j->pulse_ms = now;
    s->target = target;
    s->phase = JOINT_SETTLE;
    s->fault = "NONE";
    return NULL;
}
void joint_feed(joint_control_t *j, const uint16_t *raw,
                const uint16_t *spread, bool valid, uint32_t now)
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
        j->states[i].spread = spread ? spread[i] : 0U;
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
    if (s->angle < s->cal.limit_min - (int32_t)s->cal.tolerance ||
        s->angle > s->cal.limit_max + (int32_t)s->cal.tolerance)
    {
        return finish(j, "LIMIT", JOINT_FAULT);
    }
    int32_t error = s->target - s->angle, distance = magnitude(error);
    bool proportional = s->tune.mode == JOINT_MODE_P;
    if (distance <= (int32_t)s->cal.tolerance)
    {
        if (!j->stable)
        {
            j->stable = true;
            j->stable_ms = now;
            /* 比例式到点先短路制动，吃掉惯性，再松开滑行。 */
            if (proportional && !brake(j, now))
            {
                return finish(j, "HARDWARE", JOINT_FAULT);
            }
        }
        if (!(proportional ? release_brake(j, now) : output(j, 0)))
        {
            return finish(j, "HARDWARE", JOINT_FAULT);
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
        /* 比例式过冲换向时先制动；刚启动（last_sign 为 0）时电机本来就停着。 */
        bool ok = (proportional && j->last_sign != 0) ? brake(j, now)
                                                      : output(j, 0);
        if (!ok)
        {
            return finish(j, "HARDWARE", JOINT_FAULT);
        }
        j->last_sign = sign;
        j->switch_ms = now;
        s->phase = JOINT_SETTLE;
    }
    if ((uint32_t)(now - j->switch_ms) < REVERSE_SETTLE_MS)
    {
        if (!release_brake(j, now))
        {
            return finish(j, "HARDWARE", JOINT_FAULT);
        }
        return true;
    }
    s->phase = JOINT_MOVING;
    if (proportional)
    {
        unsigned low = s->tune.duty_min < s->cal.duty ? s->tune.duty_min
                                                      : s->cal.duty;
        uint64_t magnitude_duty =
            low + ((uint64_t)s->tune.kp * (uint32_t)distance + 500U) / 1000U;
        if (magnitude_duty > s->cal.duty)
        {
            magnitude_duty = s->cal.duty;
        }
        if (!output(j, sign * s->cal.direction * (int)magnitude_duty))
        {
            return finish(j, "HARDWARE", JOINT_FAULT);
        }
        return true;
    }
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

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (uint16_t)v);
    put_u16(&p[2], (uint16_t)(v >> 16));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)get_u16(p) | ((uint32_t)get_u16(&p[2]) << 16);
}

void joint_cal_export(const joint_control_t *j,
                      uint8_t blob[JOINT_CAL_BLOB_SIZE])
{
    memset(blob, 0, JOINT_CAL_BLOB_SIZE);
    blob[0] = (uint8_t)JOINT_CAL_BLOB_VERSION;
    blob[1] = (uint8_t)JOINT_COUNT;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        const joint_state_t *s = &j->states[i];
        uint8_t *p = &blob[4U + i * JOINT_CAL_BLOB_ENTRY_SIZE];
        if (!s->calibrated)
        {
            continue;
        }
        p[0] = 1U;
        p[1] = (uint8_t)(int8_t)s->cal.direction;
        put_u16(&p[2], (uint16_t)s->cal.duty);
        put_u16(&p[4], s->cal.raw_a);
        put_u16(&p[6], s->cal.raw_b);
        put_u32(&p[8], (uint32_t)s->cal.angle_a);
        put_u32(&p[12], (uint32_t)s->cal.angle_b);
        put_u32(&p[16], (uint32_t)s->cal.tolerance);
        put_u32(&p[20], (uint32_t)s->cal.limit_min);
        put_u32(&p[24], (uint32_t)s->cal.limit_max);
    }
}

unsigned joint_cal_import(joint_control_t *j,
                          const uint8_t blob[JOINT_CAL_BLOB_SIZE])
{
    if (blob[0] != JOINT_CAL_BLOB_VERSION || blob[1] != JOINT_COUNT)
    {
        return 0U;
    }
    unsigned restored = 0U;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        const uint8_t *p = &blob[4U + i * JOINT_CAL_BLOB_ENTRY_SIZE];
        if (p[0] != 1U)
        {
            continue;
        }
        joint_cal_t cal = {
            .raw_a = get_u16(&p[4]),
            .raw_b = get_u16(&p[6]),
            .angle_a = (int32_t)get_u32(&p[8]),
            .angle_b = (int32_t)get_u32(&p[12]),
            .direction = (int8_t)p[1],
            .duty = get_u16(&p[2]),
            .tolerance = get_u32(&p[16]),
            .limit_min = (int32_t)get_u32(&p[20]),
            .limit_max = (int32_t)get_u32(&p[24]),
        };
        if (joint_calibrate(j, i, cal) == NULL)
        {
            ++restored;
        }
    }
    return restored;
}
