#include "joint_control.h"

#include <math.h>
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

/** @brief 序列化中版本 2 每个关节的字节数，控制参数从这里开始。 */
#define BLOB_V2_ENTRY_SIZE (28U)

/** @brief 序列化中版本 4 每个关节的字节数，PI 参数从这里开始。 */
#define BLOB_V4_ENTRY_SIZE (44U)

/** @brief 序列化中版本 3 每个关节的字节数，逼近参数从这里开始。 */
#define BLOB_V3_ENTRY_SIZE (36U)

_Static_assert(JOINT_BRAKE_MAX_MS <= JOINT_REACHED_HOLD_MS,
               "brake must end before the reached hold expires");

/** @brief 背隙和超调量的上限，毫度。 */
#define TRAJ_BACKLASH_LIMIT (30000)

/** @brief 速度前馈系数的上限，千分比/(度/秒)。 */
#define TRAJ_KV_LIMIT (1000U)

/** @brief 轨迹式各关节的默认参数，见 app_config.h。 */
static const int k_traj_approach[JOINT_COUNT] = JOINT_APPROACH_DIR;
static const int32_t k_traj_backlash[JOINT_COUNT] = JOINT_BACKLASH_MDEG;
static const int32_t k_traj_overshoot[JOINT_COUNT] = JOINT_OVERSHOOT_MDEG;
static const unsigned k_traj_kv[JOINT_COUNT] = JOINT_TRAJ_KV;

/** @brief 比例积分式的逐关节默认参数，见 app_config.h 的说明。 */
static const uint16_t k_pi_kp_fast[JOINT_COUNT][2] = JOINT_PI_KP_FAST_TABLE;
static const uint16_t k_pi_kp_slow[JOINT_COUNT][2] = JOINT_PI_KP_SLOW_TABLE;
static const uint16_t k_pi_ki[JOINT_COUNT][2] = JOINT_PI_KI_TABLE;
static const uint16_t k_pi_i_max[JOINT_COUNT][2] = JOINT_PI_I_MAX_TABLE;
static const uint16_t k_pi_duty_min[JOINT_COUNT][2] = JOINT_PI_DUTY_MIN_TABLE;
static const uint16_t k_pi_duty_max[JOINT_COUNT][2] = JOINT_PI_DUTY_MAX_TABLE;

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
           c->duty >= MOTOR_DUTY_MIN && c->duty <= MOTOR_DUTY_FULL &&
           c->tolerance >= JOINT_TOLERANCE_MIN &&
           c->tolerance <= JOINT_TOLERANCE_MAX;
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
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        j->states[i].fault = "NONE";
        joint_tune_t tune = {.mode = JOINT_DEFAULT_MODE,
                             .kp = JOINT_DEFAULT_KP,
                             .duty_min = JOINT_DEFAULT_DUTY_MIN,
                             .brake_ms = JOINT_DEFAULT_BRAKE_MS};
        joint_pi_defaults(&tune.pi, i, MOTOR_DUTY_LIMIT);
        j->states[i].tune = tune;
        j->states[i].traj =
            (joint_traj_cfg_t){k_traj_approach[i], k_traj_backlash[i],
                               k_traj_overshoot[i], k_traj_kv[i]};
    }
}
bool joint_any_active(const joint_control_t *j)
{
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        if (j->states[i].motion.active || j->states[i].motion.holding)
        {
            return true;
        }
    }
    return false;
}
static bool output(joint_control_t *j, unsigned i, int duty)
{
    joint_state_t *s = &j->states[i];
    if (s->duty == duty && !s->motion.braking)
    {
        return true;
    }
    if (!j->hw.drive(i, duty))
    {
        return false;
    }
    s->duty = duty;
    s->motion.braking = false;
    return true;
}
/** @brief 开始短路制动；没有制动回调或制动时长为 0 时改为滑行。 */
static bool brake(joint_control_t *j, unsigned i, uint32_t now)
{
    joint_state_t *s = &j->states[i];
    if (!j->hw.brake || s->tune.brake_ms == 0U)
    {
        return output(j, i, 0);
    }
    if (s->motion.braking)
    {
        return true;
    }
    if (!j->hw.brake(i))
    {
        return false;
    }
    s->duty = 0;
    s->motion.braking = true;
    s->motion.brake_ms = now;
    return true;
}
/** @brief 制动时间到就松开为滑行。 */
static bool release_brake(joint_control_t *j, unsigned i, uint32_t now)
{
    const joint_state_t *s = &j->states[i];
    if (s->motion.braking &&
        (uint32_t)(now - s->motion.brake_ms) >= s->tune.brake_ms)
    {
        return output(j, i, 0);
    }
    return true;
}
/** @brief 回报一条运动命令的结果。 */
static void report_result(joint_control_t *j, unsigned i, unsigned seq,
                          const char *result)
{
    char line[80];
    (void)snprintf(line, sizeof(line), "JRESULT %u M%u %s\r\n", seq, i + 1U,
                   result);
    j->hw.reply(line);
}
/** @brief 故障名对应的故障位；不是故障时为 0。 */
static uint8_t fault_bit(const char *result)
{
    static const struct
    {
        const char *name;
        uint8_t bit;
    } bits[] = {
        {"STALL", JOINT_FAULT_STALL},     {"TIMEOUT", JOINT_FAULT_TIMEOUT},
        {"DIRECTION", JOINT_FAULT_DIRECTION}, {"LIMIT", JOINT_FAULT_LIMIT},
        {"SENSOR", JOINT_FAULT_SENSOR},   {"HARDWARE", JOINT_FAULT_HARDWARE},
    };
    for (unsigned k = 0; k < sizeof(bits) / sizeof(bits[0]); ++k)
    {
        if (!strcmp(result, bits[k].name))
        {
            return bits[k].bit;
        }
    }
    return 0U;
}

/** @brief 该关节当前读数是否可信：采样新鲜，且不是断线或短路的读数。 */
static bool sensor_ok(const joint_control_t *j, unsigned i, uint32_t now)
{
    uint16_t raw = j->states[i].raw;
    return j->valid && (uint32_t)(now - j->sample_ms) <= JOINT_SENSOR_STALE_MS &&
           raw != 0 && raw < ADC_RAW_FULL;
}

/** @brief 角度超出允许范围（限位外再加一个容差）的量，范围内为 0。 */
static int32_t limit_excess(const joint_state_t *s)
{
    int32_t low = s->cal.limit_min - (int32_t)s->cal.tolerance;
    int32_t high = s->cal.limit_max + (int32_t)s->cal.tolerance;
    if (s->angle < low)
    {
        return low - s->angle;
    }
    return s->angle > high ? s->angle - high : 0;
}

/**
 * @brief 运动中是否越限：超出量大于本次运动允许的量。
 *
 * @note 减速箱自锁，关节停在限位外时只能靠电机转回来，所以从限位外启动的
 *       运动允许保持启动时的超出量。往回走时允许量跟着收紧，只留一个容差
 *       吸收读数噪声；回到范围内后允许量为 0，和从范围内启动一样。
 */
static bool out_of_limits(joint_state_t *s)
{
    joint_motion_t *m = &s->motion;
    int32_t excess = limit_excess(s);
    if (excess > m->limit_excess)
    {
        return true;
    }
    int32_t allow = excess > 0 ? excess + (int32_t)s->cal.tolerance : 0;
    m->limit_excess = allow < m->limit_excess ? allow : m->limit_excess;
    return false;
}

static bool finish(joint_control_t *j, unsigned i, const char *result,
                   joint_phase_t phase)
{
    bool ok = j->hw.drive(i, 0);
    joint_state_t *s = &j->states[i];
    s->duty = 0;
    s->motion.braking = false;
    s->motion.active = false;
    if (!ok)
    {
        result = "HARDWARE";
        phase = JOINT_FAULT;
    }
    s->phase = phase;
    s->fault = phase == JOINT_FAULT ? result : "NONE";
    if (phase == JOINT_FAULT)
    {
        s->faults |= fault_bit(result);
    }
    report_result(j, i, s->motion.seq, result);
    return ok && phase != JOINT_FAULT;
}
uint8_t joint_fault_flags(const joint_control_t *j)
{
    uint8_t flags = 0U;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        flags |= j->states[i].faults;
    }
    return flags;
}

void joint_clear_faults(joint_control_t *j, uint32_t now)
{
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        joint_state_t *s = &j->states[i];
        if (s->faults == 0U || s->motion.active || s->motion.holding)
        {
            continue;
        }
        uint8_t keep = 0U;
        if ((s->faults & JOINT_FAULT_SENSOR) && !sensor_ok(j, i, now))
        {
            keep |= JOINT_FAULT_SENSOR;
        }
        if ((s->faults & JOINT_FAULT_HARDWARE) && !j->hw.drive(i, 0))
        {
            keep |= JOINT_FAULT_HARDWARE;
        }
        s->faults = keep;
        if (keep == 0U && s->phase == JOINT_FAULT)
        {
            s->phase = JOINT_IDLE;
            s->fault = "NONE";
        }
    }
}

/** @brief 运动中或到位后监视漂移期间都算忙，参数不能改。 */
static bool is_busy(const joint_state_t *s)
{
    return s->motion.active || s->motion.holding;
}

bool joint_cancel(joint_control_t *j)
{
    bool ok = true;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        joint_state_t *s = &j->states[i];
        if (s->motion.active)
        {
            if (!finish(j, i, "CANCELLED", JOINT_IDLE))
            {
                ok = false;
            }
        }
        else if (s->motion.holding)
        {
            /* 到位后的漂移监视不是正式运动，不发 JRESULT，只停输出。 */
            s->motion.holding = false;
            s->phase = JOINT_IDLE;
            if (!j->hw.drive(i, 0))
            {
                ok = false;
            }
            s->duty = 0;
        }
    }
    return ok;
}
const char *joint_calibrate(joint_control_t *j, unsigned index, joint_cal_t c)
{
    if (index >= JOINT_COUNT || !joint_cal_valid(&c))
    {
        return "CAL_PARAM";
    }
    joint_state_t *s = &j->states[index];
    if (is_busy(s))
    {
        return "BUSY";
    }
    s->motion.holding = false;
    s->cal = c;
    s->calibrated = true;
    s->phase = JOINT_IDLE;
    s->fault = "NONE";
    if (j->valid)
    {
        s->angle = joint_angle(&c, s->raw);
    }
    s->target = s->angle;
    s->has_target = j->valid;
    /* 换算关系变了，旧角度样本不能再用来估计转速。 */
    s->est.count = 0U;
    s->est.moving = false;
    s->speed = 0;
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
    if (is_busy(s))
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

/** @brief PI 参数是否合法；不检查与 cal.duty 的关系，运行时再压上限。 */
static bool pi_is_valid(const joint_pi_t *p)
{
    if (p->flags != 0U || p->run_pct < 10U || p->run_pct > 100U ||
        p->slow_band > 45000U)
    {
        return false;
    }
    for (unsigned d = 0U; d < 2U; ++d)
    {
        if (p->kp_fast[d] > JOINT_KP_MAX || p->kp_slow[d] > JOINT_KP_MAX ||
            p->ki[d] > JOINT_KP_MAX || p->i_max[d] > MOTOR_DUTY_LIMIT ||
            p->duty_min[d] < MOTOR_DUTY_MIN ||
            p->duty_min[d] > p->duty_max[d] ||
            p->duty_max[d] > MOTOR_DUTY_LIMIT)
        {
            return false;
        }
    }
    return true;
}

void joint_pi_defaults(joint_pi_t *pi, unsigned index, unsigned duty_max)
{
    joint_pi_t p = {0};
    unsigned cap = duty_max < MOTOR_DUTY_LIMIT ? duty_max : MOTOR_DUTY_LIMIT;
    if (index >= JOINT_COUNT)
    {
        index = 0U;
    }
    for (unsigned d = 0U; d < 2U; ++d)
    {
        unsigned high = k_pi_duty_max[index][d];
        high = high < cap ? high : cap;
        high = high >= MOTOR_DUTY_MIN ? high : MOTOR_DUTY_MIN;
        unsigned low = k_pi_duty_min[index][d];
        low = low < high ? low : high;
        low = low >= MOTOR_DUTY_MIN ? low : MOTOR_DUTY_MIN;
        p.kp_fast[d] = k_pi_kp_fast[index][d];
        p.kp_slow[d] = k_pi_kp_slow[index][d];
        p.ki[d] = k_pi_ki[index][d];
        p.i_max[d] = k_pi_i_max[index][d];
        p.duty_min[d] = (uint16_t)low;
        p.duty_max[d] = (uint16_t)high;
    }
    p.slow_band = JOINT_PI_SLOW_BAND;
    p.run_pct = JOINT_PI_RUN_PCT;
    p.flags = 0U;
    *pi = p;
}

const char *joint_set_pi(joint_control_t *j, unsigned index,
                         const joint_pi_t *pi)
{
    if (index >= JOINT_COUNT || pi == NULL)
    {
        return "JOINT_ID";
    }
    if (!pi_is_valid(pi))
    {
        return "TUNE_PARAM";
    }
    if (is_busy(&j->states[index]))
    {
        return "BUSY";
    }
    j->states[index].tune.pi = *pi;
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
    if (tune.mode == JOINT_MODE_PI && !pi_is_valid(&tune.pi))
    {
        return "TUNE_PARAM";
    }
    if (is_busy(&j->states[index]))
    {
        return "BUSY";
    }
    j->states[index].tune = tune;
    return NULL;
}

const char *joint_set_traj(joint_control_t *j, unsigned index,
                           joint_traj_cfg_t cfg)
{
    if (index >= JOINT_COUNT)
    {
        return "JOINT_ID";
    }
    if (cfg.approach < -1 || cfg.approach > 1 || cfg.backlash < 0 ||
        cfg.backlash > TRAJ_BACKLASH_LIMIT || cfg.overshoot < cfg.backlash ||
        cfg.overshoot > TRAJ_BACKLASH_LIMIT || cfg.kv > TRAJ_KV_LIMIT)
    {
        return "TUNE_PARAM";
    }
    if (is_busy(&j->states[index]))
    {
        return "BUSY";
    }
    j->states[index].traj = cfg;
    return NULL;
}

const char *joint_set_duty_max(joint_control_t *j, unsigned index,
                               unsigned duty)
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
    if (duty == 0U || !motor_duty_valid(index, (int)duty))
    {
        return "TUNE_PARAM";
    }
    if (is_busy(s))
    {
        return "BUSY";
    }
    s->cal.duty = duty;
    return NULL;
}
uint16_t joint_median3(uint16_t a, uint16_t b, uint16_t c)
{
    if (a > b)
    {
        uint16_t t = a;
        a = b;
        b = t;
    }
    /* 此时 a <= b：中值是 b、c 中较小者与 a 中较大的那个。 */
    uint16_t low = (b < c) ? b : c;
    return (a > low) ? a : low;
}
/** @brief 本关节允许输出的最大占空比：标定值与全局上限取小。 */
static unsigned duty_max(const joint_state_t *s)
{
    return s->cal.duty < MOTOR_DUTY_SMALL_MAX ? s->cal.duty
                                              : MOTOR_DUTY_SMALL_MAX;
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
/** @brief 轨迹式一次运动的规划结果，确认可行后才写进 joint_motion_t。 */
typedef struct
{
    joint_traj_t seg[2];
    unsigned seg_count;
    bool skipped; /**< 需要单向逼近但超调量被限位压缩到小于背隙，已放弃。 */
} traj_plan_t;

/** @brief 正在运动的轨迹式关节在 now 时刻的参考状态。 */
static joint_traj_point_t traj_point(const joint_motion_t *m, uint32_t now)
{
    return joint_traj_eval(&m->seg[m->seg_index], (float)(now - m->seg_ms));
}

/**
 * @brief 单向逼近：从 from 去 target 是否要先越过目标到中间点。
 *
 * 规定了逼近方向、距离超过容差、而这次的运动方向与规定方向相反时，中间点
 * 取在目标另一侧 overshoot 处，压到允许范围内；压完后离目标不足 backlash
 * 时放弃单向逼近，*skipped 置 true。
 *
 * @return true 表示要先去 *via。
 */
static bool approach_via(const joint_state_t *s, int32_t from, int32_t target,
                         int32_t *via, bool *skipped)
{
    int approach = s->traj.approach;
    int32_t distance = target - from;
    *skipped = false;
    if (approach == 0 || magnitude(distance) <= (int32_t)s->cal.tolerance ||
        (distance > 0 ? 1 : -1) == approach)
    {
        return false;
    }
    /* 中间点在目标的另一侧：沿本次运动方向越过目标，再按规定方向回来。 */
    int32_t point = target - approach * s->traj.overshoot;
    point = point < s->cal.limit_min ? s->cal.limit_min : point;
    point = point > s->cal.limit_max ? s->cal.limit_max : point;
    if (magnitude(point - target) < s->traj.backlash || point == target)
    {
        *skipped = true;
        return false;
    }
    *via = point;
    return true;
}

/**
 * @brief 从 from 规划到 target：需要单向逼近时先到中间点，再以
 *        JOINT_RETURN_SPEED_MDEG_S 回到目标。
 */
static void plan_traj(const joint_state_t *s, joint_traj_point_t from,
                      int32_t target, int32_t speed, traj_plan_t *out)
{
    float vmax = (float)JOINT_TRAJ_VMAX_MDEG_S;
    if (speed > 0 && (float)speed < vmax)
    {
        vmax = (float)speed;
    }
    float amax = (float)JOINT_TRAJ_AMAX_MDEG_S2;
    int32_t via = target;
    out->seg_count = 1U;
    if (approach_via(s, (int32_t)lroundf(from.p), target, &via, &out->skipped))
    {
        float back = (float)JOINT_RETURN_SPEED_MDEG_S;
        joint_traj_plan(&out->seg[0], from, (float)via, vmax, amax,
                        JOINT_TRAJ_MIN_MS);
        joint_traj_point_t rest = {(float)via, 0.0f, 0.0f};
        joint_traj_plan(&out->seg[1], rest, (float)target,
                        back < vmax ? back : vmax, amax,
                        JOINT_TRAJ_MIN_MS);
        out->seg_count = 2U;
        return;
    }
    joint_traj_plan(&out->seg[0], from, (float)target, vmax, amax,
                    JOINT_TRAJ_MIN_MS);
}

const char *joint_move(joint_control_t *j, unsigned seq, unsigned index,
                       int32_t target, uint32_t timeout, uint32_t now)
{
    return joint_move_speed(j, seq, index, target, timeout, 0, now);
}

const char *joint_move_speed(joint_control_t *j, unsigned seq, unsigned index,
                             int32_t target, uint32_t timeout, int32_t speed,
                             uint32_t now)
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
        timeout < JOINT_MOVE_TIMEOUT_MIN_MS ||
        timeout > JOINT_MOVE_TIMEOUT_MAX_MS)
    {
        return "TARGET_RANGE";
    }
    /* 当前角度在限位外不拒绝：减速箱自锁，不让动就永远回不来。越限保护
     * 改在运动中只拦继续往外走，见 out_of_limits。 */
    if (s->raw == 0 || s->raw >= ADC_RAW_FULL)
    {
        return "POSITION_RANGE";
    }
    joint_motion_t *m = &s->motion;
    bool is_traj = s->tune.mode == JOINT_MODE_TRAJ;
    traj_plan_t plan;
    int32_t via = target;
    bool has_via = false, skipped = false;
    if (!is_traj)
    {
        has_via = approach_via(s, s->angle, target, &via, &skipped);
    }
    if (is_traj)
    {
        /* 运动中换目标时从当前参考状态接着走，否则从实际位置静止出发。 */
        joint_traj_point_t from = {(float)s->angle, 0.0f, 0.0f};
        if (m->active && m->seg_count > 0U)
        {
            from = traj_point(m, now);
        }
        plan_traj(s, from, target, speed, &plan);
        uint32_t total = JOINT_REACHED_HOLD_MS;
        for (unsigned k = 0; k < plan.seg_count; ++k)
        {
            total += plan.seg[k].duration_ms;
        }
        if (total > timeout)
        {
            return "TOO_SLOW";
        }
    }
    if (m->active)
    {
        /* 新目标替代旧目标：方向、制动和换向计时沿用，避免带电直接反转。 */
        report_result(j, index, m->seq, "SUPERSEDED");
    }
    else
    {
        m->last_sign = 0;
        m->braking = false;
        m->switch_ms = now;
        m->duty_cmd = 0;
        m->kick = s->tune.mode == JOINT_MODE_PI;
        m->kick_ms = 0U;
    }
    /* 到位后的漂移监视被新命令接管；积分从零开始，避免旧目标的累积量。 */
    m->holding = false;
    m->integral = 0.0f;
    m->active = true;
    m->seq = seq;
    m->start_ms = now;
    m->progress_ms = now;
    m->timeout_ms = timeout;
    m->stable = false;
    m->goal = has_via ? via : target;
    m->via_pending = has_via;
    m->limit_excess = limit_excess(s);
    m->initial_error = magnitude(m->goal - s->angle);
    m->best_error = m->initial_error;
    m->pulse_on = false;
    m->near_mode = false;
    m->pulse_ms = now;
    s->target = target;
    s->has_target = true;
    s->phase = JOINT_SETTLE;
    s->fault = "NONE";
    m->seg_count = 0U;
    if (is_traj)
    {
        memcpy(m->seg, plan.seg, sizeof(m->seg));
        m->seg_count = plan.seg_count;
        m->seg_index = 0U;
        m->seg_ms = now;
        m->seg_angle = s->angle;
        m->ref = (int32_t)lroundf(plan.seg[0].p0);
        m->ref_speed = (int32_t)lroundf(plan.seg[0].v0);
        m->integral = 0.0f;
        m->tick_ms = now;
        s->phase = JOINT_MOVING;
        skipped = plan.skipped;
    }
    if (skipped)
    {
        char line[48];
        (void)snprintf(line, sizeof(line), "JNOTE %u M%u NO_OVERSHOOT\r\n", seq,
                       index + 1U);
        j->hw.reply(line);
    }
    return NULL;
}

int32_t joint_reference(const joint_state_t *s)
{
    if (!s->motion.active)
    {
        return s->target;
    }
    return s->motion.seg_count > 0U ? s->motion.ref : s->motion.goal;
}

/** @brief est 中从最旧样本数起第 k 个样本的下标。 */
static unsigned speed_index(const joint_speed_t *e, unsigned k)
{
    return (e->head + JOINT_SPEED_WINDOW - e->count + k) % JOINT_SPEED_WINDOW;
}

/**
 * @brief 加入一次角度采样并更新转速估计。
 *
 * 窗口未满时转速报 0；窗口满后用两端各 JOINT_SPEED_EDGE 个样本的平均角度
 * 和平均时刻求斜率，再按迟滞阈值判定是否静止。
 */
static void update_speed(joint_state_t *s, uint32_t now)
{
    joint_speed_t *e = &s->est;
    e->angle[e->head] = s->angle;
    e->ms[e->head] = now;
    e->head = (e->head + 1U) % JOINT_SPEED_WINDOW;
    if (e->count < JOINT_SPEED_WINDOW)
    {
        ++e->count;
    }
    if (e->count < JOINT_SPEED_WINDOW)
    {
        s->speed = 0;
        return;
    }
    /* 时刻相对最旧样本计算，避免 HAL_GetTick() 回绕。 */
    uint32_t base = e->ms[speed_index(e, 0U)];
    int64_t old_angle = 0, new_angle = 0, old_ms = 0, new_ms = 0;
    for (unsigned k = 0; k < JOINT_SPEED_EDGE; ++k)
    {
        unsigned a = speed_index(e, k);
        unsigned b = speed_index(e, JOINT_SPEED_WINDOW - JOINT_SPEED_EDGE + k);
        old_angle += e->angle[a];
        new_angle += e->angle[b];
        old_ms += (uint32_t)(e->ms[a] - base);
        new_ms += (uint32_t)(e->ms[b] - base);
    }
    int64_t span = new_ms - old_ms; /* 两组时刻之和的差，同样放大了 EDGE 倍 */
    int64_t speed = span > 0 ? (new_angle - old_angle) * 1000 / span : 0;
    int64_t size = speed < 0 ? -speed : speed;
    if (e->moving ? size < JOINT_SPEED_ZERO_MDEG_S
                  : size > JOINT_SPEED_MOVE_MDEG_S)
    {
        e->moving = !e->moving;
    }
    s->speed = e->moving ? (int32_t)speed : 0;
}

/** @brief 样本中断：清空窗口，转速报 0。 */
static void reset_speed(joint_state_t *s)
{
    s->est.count = 0U;
    s->est.moving = false;
    s->speed = 0;
}
void joint_feed(joint_control_t *j, const uint16_t *raw, const uint16_t *spread,
                bool valid, uint32_t now)
{
    j->valid = valid;
    if (!valid)
    {
        for (unsigned i = 0; i < JOINT_COUNT; ++i)
        {
            reset_speed(&j->states[i]);
        }
        return;
    }
    j->sample_ms = now;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        joint_state_t *s = &j->states[i];
        s->raw = raw[i];
        s->spread = spread ? spread[i] : 0U;
        if (!s->calibrated)
        {
            reset_speed(s);
            continue;
        }
        s->angle = joint_angle(&s->cal, raw[i]);
        if (!s->has_target)
        {
            s->target = s->angle;
            s->has_target = true;
        }
        if (raw[i] == 0 || raw[i] >= ADC_RAW_FULL)
        {
            reset_speed(s);
        }
        else
        {
            update_speed(s, now);
        }
    }
}
/**
 * @brief 轨迹式的一步控制：沿轨迹取参考点，输出前馈加 PI 反馈。
 *
 * 前馈 = kv × 参考速度 + ka × 参考加速度 + 摩擦补偿。摩擦补偿按参考速度而不是
 * 测量速度计算，方向取参考速度的方向，静止起步时取本段的运动方向：零速时为
 * 起转占空比减 JOINT_TRAJ_PRELOAD_MARGIN，即预加到略低于起转点，电机还不会
 * 动；随参考速度增大平滑过渡到动摩擦占空比。轨迹走完后只保留朝目标方向的
 * 预加量，进入容差后输出 0 滑行，不短路制动，保持 JOINT_REACHED_HOLD_MS 判定
 * 到达。跟踪误差持续超过 JOINT_TRACK_ERROR_MAX 判定停滞，实际角度反着参考
 * 走出 2 倍容差加余量判定方向错误。
 */
static bool tick_traj(joint_control_t *j, unsigned i, uint32_t now)
{
    joint_state_t *s = &j->states[i];
    joint_motion_t *m = &s->motion;
    const joint_traj_t *g = &m->seg[m->seg_index];
    if (m->seg_index + 1U < m->seg_count &&
        (uint32_t)(now - m->seg_ms) >= g->duration_ms)
    {
        /* 进入下一段：两段在中间点都是静止的，参考状态仍然连续。 */
        m->seg_ms += g->duration_ms;
        ++m->seg_index;
        m->seg_angle = s->angle;
        m->integral = 0.0f;
        g = &m->seg[m->seg_index];
    }
    uint32_t elapsed = now - m->seg_ms;
    joint_traj_point_t r = joint_traj_eval(g, (float)elapsed);
    m->ref = (int32_t)lroundf(r.p);
    m->ref_speed = (int32_t)lroundf(r.v);
    bool is_final =
        m->seg_index + 1U == m->seg_count && elapsed >= g->duration_ms;
    float dt = (float)(now - m->tick_ms) / 1000.0f;
    m->tick_ms = now;
    if (dt > 0.05f)
    {
        dt = 0.05f;
    }
    s->phase = JOINT_MOVING;

    float error = r.p - (float)s->angle;
    if (fabsf(error) <= (float)JOINT_TRACK_ERROR_MAX)
    {
        m->progress_ms = now;
    }
    else
    {
        int32_t moved = s->angle - m->seg_angle;
        int32_t wrong = error > 0.0f ? -moved : moved;
        if (wrong > (int32_t)(2U * s->cal.tolerance + JOINT_DIRECTION_MARGIN))
        {
            return finish(j, i, "DIRECTION", JOINT_FAULT);
        }
        if ((uint32_t)(now - m->progress_ms) >= JOINT_STALL_MS)
        {
            return finish(j, i, "STALL", JOINT_FAULT);
        }
    }

    if (is_final && magnitude(s->target - s->angle) <= (int32_t)s->cal.tolerance)
    {
        if (!m->stable)
        {
            m->stable = true;
            m->stable_ms = now;
            m->integral = 0.0f;
        }
        if (!output(j, i, 0))
        {
            return finish(j, i, "HARDWARE", JOINT_FAULT);
        }
        if ((uint32_t)(now - m->stable_ms) >= JOINT_REACHED_HOLD_MS)
        {
            return finish(j, i, "REACHED", JOINT_DONE);
        }
        return true;
    }
    m->stable = false;

    int seg_dir = g->p1 > g->p0 ? 1 : (g->p1 < g->p0 ? -1 : 0);
    float speed = fabsf(r.v);
    float d_break = (float)s->tune.duty_min;
    float d_pre = d_break - (float)JOINT_TRAJ_PRELOAD_MARGIN;
    d_pre = d_pre > 0.0f ? d_pre : 0.0f;
    float d_kin = d_break * (float)JOINT_TRAJ_KINETIC_PCT / 100.0f;
    float friction = 0.0f;
    if (!is_final)
    {
        int dir = speed > 1.0f ? (r.v > 0.0f ? 1 : -1) : seg_dir;
        float ramp = speed / (float)JOINT_TRAJ_FRICTION_SPEED;
        ramp = ramp < 1.0f ? ramp : 1.0f;
        friction = (float)dir * (d_pre + (d_kin - d_pre) * ramp);
    }
    else if ((error > 0.0f ? 1 : -1) == seg_dir)
    {
        /* 轨迹走完还没进容差：保留朝目标方向的预加量，让 PI 小幅推过去。 */
        friction = (float)seg_dir * d_pre;
    }

    m->integral += (float)JOINT_TRAJ_KI * error / 1000.0f * dt;
    float i_max = (float)JOINT_TRAJ_I_MAX;
    m->integral = m->integral > i_max ? i_max : m->integral;
    m->integral = m->integral < -i_max ? -i_max : m->integral;

    float u = friction + (float)s->traj.kv * r.v / 1000.0f +
              (float)JOINT_TRAJ_KA * r.a / 1000.0f +
              (float)s->tune.kp * error / 1000.0f + m->integral;
    float high = (float)duty_max(s);
    u = u > high ? high : u;
    u = u < -high ? -high : u;
    int duty = (int)lroundf(u);
    /* 低于电机最小占空比的输出本来也转不动，置 0 以满足驱动层的范围检查。 */
    if (duty > -MOTOR_DUTY_MIN && duty < MOTOR_DUTY_MIN)
    {
        duty = 0;
    }
    if (!output(j, i, duty * s->cal.direction))
    {
        return finish(j, i, "HARDWARE", JOINT_FAULT);
    }
    return true;
}

/** @brief 按误差方向选择分方向参数的下标：0=角度增大，1=角度减小。 */
static unsigned pi_index(const joint_state_t *s, int32_t error)
{
    int dir = error > 0 ? 1 : -1;
    return (dir * s->cal.direction > 0) ? 0U : 1U;
}

/** @brief 该方向的占空比上限，同时受标定上限和全局上限约束。 */
static unsigned pi_cap(const joint_state_t *s, unsigned idx)
{
    unsigned cap = s->tune.pi.duty_max[idx];
    unsigned high = duty_max(s);
    return cap < high ? cap : high;
}

/** @brief 执行一拍 PI：前馈 + 分方向 P + 条件积分 + 斜坡，返回 false 表示输出失败。 */
static bool pi_apply(joint_control_t *j, unsigned i, int32_t error, uint32_t now,
                     unsigned cap_pct, bool allow_kick)
{
    joint_state_t *s = &j->states[i];
    joint_motion_t *m = &s->motion;
    const joint_pi_t *p = &s->tune.pi;
    int32_t distance = magnitude(error);
    int sign = error > 0 ? 1 : -1;
    unsigned idx = pi_index(s, error);

    float dt = (float)(uint32_t)(now - m->tick_ms) / 1000.0f;
    m->tick_ms = now;
    if (dt < 0.0f)
    {
        dt = 0.0f;
    }
    if (dt > 0.05f)
    {
        dt = 0.05f;
    }

    unsigned cap = pi_cap(s, idx) * cap_pct / 100U;
    if (distance <= (int32_t)p->slow_band)
    {
        cap = cap * JOINT_PI_SLOW_CAP_PCT / 100U;
    }
    if (cap > duty_max(s))
    {
        cap = duty_max(s);
    }

    /* 前馈：静止起步用 duty_min 顶开静摩擦（kick 窗口），之后退回维持量。 */
    int ff;
    unsigned run = (unsigned)p->duty_min[idx] * p->run_pct / 100U;
    if (allow_kick && m->kick)
    {
        if (m->kick_ms == 0U)
        {
            m->kick_ms = now;
        }
        if ((uint32_t)(now - m->kick_ms) < JOINT_PI_KICK_MS)
        {
            ff = (int)p->duty_min[idx];
        }
        else
        {
            m->kick = false;
            ff = (int)run;
        }
    }
    else
    {
        ff = (int)run;
    }

    unsigned kp = (distance > (int32_t)p->slow_band) ? p->kp_fast[idx] : p->kp_slow[idx];

    /* 条件积分：输出饱和且误差仍同向时回退本拍积分，避免越绕越深。 */
    float old_i = m->integral;
    m->integral += (float)p->ki[idx] * (float)error / 1000.0f * dt;
    float i_max = (float)p->i_max[idx];
    m->integral = m->integral > i_max ? i_max : m->integral;
    m->integral = m->integral < -i_max ? -i_max : m->integral;

    /* 统一在带符号域计算：前馈、P、积分同号相加才是朝目标的合力。 */
    float u = (float)sign * (float)ff + (float)kp * (float)error / 1000.0f +
              m->integral;
    if (fabsf(u) > (float)cap && ((u > 0.0f) == (error > 0)))
    {
        m->integral = old_i;
        u = (float)sign * (float)ff + (float)kp * (float)error / 1000.0f +
            m->integral;
    }
    /* 远离目标时不允许输出低于起转占空比：否则积分会把合力压到摩擦区以下，
     * 电机停转、误差不变、积分继续反向累积，最后必然报 STALL。 */
    if (distance > (int32_t)s->cal.tolerance + 500 &&
        fabsf(u) < (float)p->duty_min[idx])
    {
        u = (float)sign * (float)p->duty_min[idx];
        m->integral = old_i;
    }
    if (u > (float)cap)
    {
        u = (float)cap;
    }
    if (u < -(float)cap)
    {
        u = -(float)cap;
    }

    int duty = s->cal.direction * (int)lroundf(u);
    int step = (int)((float)JOINT_PI_SLEW_PERMILLE_S * dt);
    if (step < 1)
    {
        step = 1;
    }
    if (duty > m->duty_cmd + step)
    {
        duty = m->duty_cmd + step;
    }
    if (duty < m->duty_cmd - step)
    {
        duty = m->duty_cmd - step;
    }
    m->duty_cmd = duty;
    s->phase = JOINT_MOVING;

    int out = duty;
    if (out > -MOTOR_DUTY_MIN && out < MOTOR_DUTY_MIN)
    {
        out = 0;
    }
    if (!output(j, i, out))
    {
        return finish(j, i, "HARDWARE", JOINT_FAULT);
    }
    return true;
}

/** @brief PI 运动的一拍：自适应制动 + PI。 */
static bool tick_pi(joint_control_t *j, unsigned i, uint32_t now)
{
    joint_state_t *s = &j->states[i];
    joint_motion_t *m = &s->motion;
    int32_t error = m->goal - s->angle;
    int32_t distance = magnitude(error);
    int sign = error > 0 ? 1 : -1;
    if (distance <= (int32_t)s->cal.tolerance)
    {
        return true;
    }
    /* 速度足够大且仍是朝目标的方向时，按“制动+滑行”距离提前刹车。 */
    int32_t speed = s->speed;
    if (!m->braking && magnitude(speed) > JOINT_PI_BRAKE_SPEED &&
        m->last_sign == sign)
    {
        int32_t stop = (int32_t)s->cal.tolerance +
                       (int32_t)((int64_t)magnitude(speed) *
                                 (int64_t)(s->tune.brake_ms + JOINT_PI_COAST_MS) / 1000) +
                       JOINT_PI_BRAKE_MARGIN;
        if (distance <= stop)
        {
            if (!brake(j, i, now))
            {
                return finish(j, i, "HARDWARE", JOINT_FAULT);
            }
            m->integral = 0.0f;
            m->duty_cmd = 0;
        }
    }
    if (m->braking)
    {
        if (!release_brake(j, i, now))
        {
            return finish(j, i, "HARDWARE", JOINT_FAULT);
        }
        if (m->braking)
        {
            return true;
        }
        /* 松刹后重新计时，避免把制动期间的时间算进积分。 */
        m->tick_ms = now;
    }
    return pi_apply(j, i, error, now, 100U, true);
}

/** @brief REACHED 之后的漂移监视：容差外才用限幅 PI 纠回，不产生新的 JRESULT。 */
static bool tick_hold(joint_control_t *j, unsigned i, uint32_t now)
{
    joint_state_t *s = &j->states[i];
    joint_motion_t *m = &s->motion;
    if (s->tune.mode != JOINT_MODE_PI)
    {
        m->holding = false;
        s->phase = JOINT_DONE;
        return true;
    }
    if (!sensor_ok(j, i, now))
    {
        m->holding = false;
        return finish(j, i, "SENSOR", JOINT_FAULT);
    }
    if (out_of_limits(s))
    {
        m->holding = false;
        return finish(j, i, "LIMIT", JOINT_FAULT);
    }
    int32_t error = m->goal - s->angle;
    int32_t distance = magnitude(error);
    int32_t band = (int32_t)s->cal.tolerance + (int32_t)JOINT_PI_HOLD_HYS;
    /* 监视从判到达起只持续 JOINT_PI_HOLD_MS，到时不论在不在容差内都停止
     * 输出并结束：HOLD 里没有停滞和超时保护，不能无限期地驱动下去。 */
    bool is_over = (uint32_t)(now - m->hold_ms) >= JOINT_PI_HOLD_MS;
    if (is_over || distance <= band)
    {
        if (!output(j, i, 0))
        {
            return finish(j, i, "HARDWARE", JOINT_FAULT);
        }
        if (is_over)
        {
            m->holding = false;
            s->phase = JOINT_DONE;
        }
        return true;
    }
    bool ok = pi_apply(j, i, error, now, JOINT_PI_SLOW_CAP_PCT, false);
    if (ok)
    {
        /* pi_apply 会把阶段写成 MOVING，纠偏期间对外仍是 HOLD。 */
        s->phase = JOINT_HOLD;
    }
    return ok;
}

/** @brief 推进一个运动中关节的控制；返回 false 表示产生了新故障。 */
static bool tick_one(joint_control_t *j, unsigned i, uint32_t now)
{
    joint_state_t *s = &j->states[i];
    joint_motion_t *m = &s->motion;
    if (!m->active)
    {
        /* 到位后的漂移监视走独立路径，不推进原运动；它自己查传感器和限位，
         * 并且不受运动超时约束：运动已经报过 REACHED，不能再报 TIMEOUT。 */
        return tick_hold(j, i, now);
    }
    if (!sensor_ok(j, i, now))
    {
        return finish(j, i, "SENSOR", JOINT_FAULT);
    }
    if ((uint32_t)(now - m->start_ms) >= m->timeout_ms)
    {
        return finish(j, i, "TIMEOUT", JOINT_FAULT);
    }
    if (out_of_limits(s))
    {
        return finish(j, i, "LIMIT", JOINT_FAULT);
    }
    if (m->seg_count > 0U)
    {
        return tick_traj(j, i, now);
    }
    if (m->via_pending)
    {
        /* 单向逼近：到了中间点附近或已越过，改去目标。之后误差反号，按换向
         * 处理：比例式先制动，再停 JOINT_REVERSE_SETTLE_MS 后反向。 */
        int travel = m->goal > s->target ? 1 : -1;
        if (travel * (s->angle - m->goal) >= -(int32_t)s->cal.tolerance)
        {
            m->via_pending = false;
            m->goal = s->target;
            m->stable = false;
            m->initial_error = magnitude(m->goal - s->angle);
            m->best_error = m->initial_error;
            m->progress_ms = now;
            if (s->tune.mode == JOINT_MODE_PI)
            {
                m->integral = 0.0f;
                m->kick = true;
                m->kick_ms = 0U;
                m->duty_cmd = 0;
            }
        }
    }
    int32_t error = m->goal - s->angle, distance = magnitude(error);
    bool proportional = s->tune.mode == JOINT_MODE_P;
    bool uses_brake = proportional || s->tune.mode == JOINT_MODE_PI;
    if (distance <= (int32_t)s->cal.tolerance)
    {
        if (!m->stable)
        {
            m->stable = true;
            m->stable_ms = now;
            /* 比例式到点先短路制动，吃掉惯性，再松开滑行。 */
            if (uses_brake && !brake(j, i, now))
            {
                return finish(j, i, "HARDWARE", JOINT_FAULT);
            }
        }
        if (!(uses_brake ? release_brake(j, i, now) : output(j, i, 0)))
        {
            return finish(j, i, "HARDWARE", JOINT_FAULT);
        }
        if ((uint32_t)(now - m->stable_ms) >= JOINT_REACHED_HOLD_MS)
        {
            if (!finish(j, i, "REACHED", JOINT_DONE))
            {
                return false;
            }
            if (s->tune.mode == JOINT_MODE_PI)
            {
                /* 到位后继续监视漂移，漂出容差才纠回；对外仍是 REACHED。 */
                m->holding = true;
                m->hold_ms = now;
                m->integral = 0.0f;
                s->phase = JOINT_HOLD;
            }
            return true;
        }
        return true;
    }
    m->stable = false;
    if (distance + JOINT_PROGRESS_STEP < m->best_error)
    {
        m->best_error = distance;
        m->progress_ms = now;
    }
    /* PI 只在起步窗口内判反走：短行程高速过冲会超过 2*tol+1° 的阈值，
     * 全程判会把它误报成 DIRECTION。 */
    bool direction_check =
        (s->tune.mode != JOINT_MODE_PI) ||
        ((uint32_t)(now - m->start_ms) <= JOINT_PI_DIR_CHECK_MS);
    if (direction_check &&
        distance > m->initial_error + (int32_t)(2U * s->cal.tolerance +
                                                JOINT_DIRECTION_MARGIN))
    {
        return finish(j, i, "DIRECTION", JOINT_FAULT);
    }
    if ((uint32_t)(now - m->progress_ms) >= JOINT_STALL_MS)
    {
        return finish(j, i, "STALL", JOINT_FAULT);
    }
    int sign = error > 0 ? 1 : -1;
    if (sign != m->last_sign)
    {
        /* 比例式过冲换向时先制动；刚启动（last_sign 为 0）时电机本来就停着。 */
        bool ok =
            (uses_brake && m->last_sign != 0) ? brake(j, i, now) : output(j, i, 0);
        if (!ok)
        {
            return finish(j, i, "HARDWARE", JOINT_FAULT);
        }
        m->last_sign = sign;
        m->switch_ms = now;
        if (s->tune.mode == JOINT_MODE_PI)
        {
            /* 换向后积分和前馈都要重来，占空比从 0 重新爬升。 */
            m->integral = 0.0f;
            m->kick = true;
            m->kick_ms = 0U;
            m->duty_cmd = 0;
        }
        s->phase = JOINT_SETTLE;
    }
    if ((uint32_t)(now - m->switch_ms) < JOINT_REVERSE_SETTLE_MS)
    {
        if (!release_brake(j, i, now))
        {
            return finish(j, i, "HARDWARE", JOINT_FAULT);
        }
        return true;
    }
    bool entering_move = s->phase != JOINT_MOVING;
    s->phase = JOINT_MOVING;
    if (s->tune.mode == JOINT_MODE_PI)
    {
        if (entering_move)
        {
            m->tick_ms = now;
        }
        return tick_pi(j, i, now);
    }
    if (proportional)
    {
        unsigned high = duty_max(s);
        unsigned low = s->tune.duty_min < high ? s->tune.duty_min : high;
        uint64_t magnitude_duty =
            low + ((uint64_t)s->tune.kp * (uint32_t)distance + 500U) / 1000U;
        if (magnitude_duty > high)
        {
            magnitude_duty = high;
        }
        if (!output(j, i, sign * s->cal.direction * (int)magnitude_duty))
        {
            return finish(j, i, "HARDWARE", JOINT_FAULT);
        }
        return true;
    }
    /* 接近目标时改为短脉冲点动（驱动 JOINT_PULSE_ON_MS、停 JOINT_PULSE_OFF_MS），
     * 因为低占空比电机根本转不动。 */
    bool near =
        distance <= (int32_t)(5U * s->cal.tolerance + JOINT_NEAR_MARGIN);
    if (near && !m->near_mode)
    {
        m->pulse_on = false;
        m->pulse_ms = now;
    }
    m->near_mode = near;
    if (!near)
    {
        m->pulse_on = false;
    }
    int duty = sign * s->cal.direction * (int)duty_max(s);
    if (near)
    {
        if (!m->pulse_on && (uint32_t)(now - m->pulse_ms) >= JOINT_PULSE_OFF_MS)
        {
            m->pulse_on = true;
            m->pulse_ms = now;
        }
        else if (m->pulse_on &&
                 (uint32_t)(now - m->pulse_ms) >= JOINT_PULSE_ON_MS)
        {
            m->pulse_on = false;
            m->pulse_ms = now;
        }
        if (!m->pulse_on)
        {
            duty = 0;
        }
    }
    if (!output(j, i, duty))
    {
        return finish(j, i, "HARDWARE", JOINT_FAULT);
    }
    return true;
}
bool joint_tick(joint_control_t *j, uint32_t now)
{
    bool ok = true;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        if ((j->states[i].motion.active || j->states[i].motion.holding) &&
            !tick_one(j, i, now))
        {
            ok = false;
        }
    }
    return ok;
}
const char *joint_phase_name(joint_phase_t phase)
{
    static const char *const names[JOINT_PHASE_COUNT] = {
        "IDLE", "SETTLE", "MOVE", "DONE", "FAULT", "HOLD"};
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

static void put_pi(uint8_t *p, const joint_pi_t *pi)
{
    for (unsigned d = 0; d < 2U; ++d)
    {
        uint8_t *q = &p[d * 12U];
        put_u16(&q[0], pi->kp_fast[d]);
        put_u16(&q[2], pi->kp_slow[d]);
        put_u16(&q[4], pi->ki[d]);
        put_u16(&q[6], pi->i_max[d]);
        put_u16(&q[8], pi->duty_min[d]);
        put_u16(&q[10], pi->duty_max[d]);
    }
    put_u16(&p[24], pi->slow_band);
    p[26] = pi->run_pct;
    p[27] = pi->flags;
}

static void get_pi(const uint8_t *p, joint_pi_t *pi)
{
    for (unsigned d = 0; d < 2U; ++d)
    {
        const uint8_t *q = &p[d * 12U];
        pi->kp_fast[d] = get_u16(&q[0]);
        pi->kp_slow[d] = get_u16(&q[2]);
        pi->ki[d] = get_u16(&q[4]);
        pi->i_max[d] = get_u16(&q[6]);
        pi->duty_min[d] = get_u16(&q[8]);
        pi->duty_max[d] = get_u16(&q[10]);
    }
    pi->slow_band = get_u16(&p[24]);
    pi->run_pct = p[26];
    pi->flags = p[27];
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
        uint8_t *t = &p[BLOB_V2_ENTRY_SIZE];
        t[0] = 1U;
        t[1] = (uint8_t)s->tune.mode;
        put_u16(&t[2], (uint16_t)s->tune.kp);
        put_u16(&t[4], (uint16_t)s->tune.duty_min);
        put_u16(&t[6], (uint16_t)s->tune.brake_ms);
        uint8_t *a = &p[BLOB_V3_ENTRY_SIZE];
        a[0] = 1U;
        a[1] = (uint8_t)(int8_t)s->traj.approach;
        put_u16(&a[4], (uint16_t)s->traj.backlash);
        put_u16(&a[6], (uint16_t)s->traj.overshoot);
        put_pi(&p[BLOB_V4_ENTRY_SIZE], &s->tune.pi);
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

unsigned joint_cal_import(joint_control_t *j, const uint8_t *blob,
                          uint32_t size)
{
    /* 先查长度再读版本号：旧实现先读 blob[0]，越界读在极端情况下会踩内存。 */
    if (blob == NULL || size < 4U)
    {
        return 0U;
    }
    unsigned version = blob[0];
    unsigned entry;
    bool has_tune = false, has_traj = false, has_pi = false;
    if (version == 5U && size == JOINT_CAL_BLOB_SIZE)
    {
        entry = JOINT_CAL_BLOB_ENTRY_SIZE;
        has_tune = has_traj = has_pi = true;
    }
    else if (version == 4U && size == JOINT_CAL_BLOB_V4_SIZE)
    {
        entry = BLOB_V4_ENTRY_SIZE;
        has_tune = has_traj = true;
    }
    else if (version == 3U && size == JOINT_CAL_BLOB_V3_SIZE)
    {
        entry = BLOB_V3_ENTRY_SIZE;
        has_tune = true;
    }
    else if (version == 2U && size == JOINT_CAL_BLOB_V2_SIZE)
    {
        entry = BLOB_V2_ENTRY_SIZE;
    }
    else
    {
        return 0U;
    }
    if (blob[1] != JOINT_COUNT)
    {
        return 0U;
    }
    unsigned restored = 0U;
    for (unsigned i = 0; i < JOINT_COUNT; ++i)
    {
        const uint8_t *p = &blob[4U + i * entry];
        joint_tune_t tune = j->states[i].tune;
        bool have_tune = false;
        if (has_tune)
        {
            const uint8_t *t = &p[BLOB_V2_ENTRY_SIZE];
            if (t[0] == 1U)
            {
                tune.mode = (joint_mode_t)t[1];
                tune.kp = get_u16(&t[2]);
                tune.duty_min = get_u16(&t[4]);
                tune.brake_ms = get_u16(&t[6]);
                have_tune = true;
            }
        }
        if (has_traj)
        {
            const uint8_t *a = &p[BLOB_V3_ENTRY_SIZE];
            if (a[0] == 1U)
            {
                joint_traj_cfg_t cfg = j->states[i].traj;
                cfg.approach = (int8_t)a[1];
                cfg.backlash = get_u16(&a[4]);
                cfg.overshoot = get_u16(&a[6]);
                (void)joint_set_traj(j, i, cfg);
            }
        }
        if (has_pi)
        {
            get_pi(&p[BLOB_V4_ENTRY_SIZE], &tune.pi);
        }
        else if (have_tune && tune.mode == JOINT_MODE_P &&
                 JOINT_PI_MIGRATE_LEGACY)
        {
            /* v2–v4 的 P 参数迁移成 PI 初值：kp、duty_min 两方向取同值，
             * 其余用默认表；M3/M6 的方向差异只能上台架重新整定。 */
            unsigned cap = (p[0] == 1U) ? get_u16(&p[2]) : MOTOR_DUTY_LIMIT;
            joint_pi_defaults(&tune.pi, i, cap);
            for (unsigned d = 0; d < 2U; ++d)
            {
                tune.pi.kp_fast[d] = tune.kp;
                tune.pi.kp_slow[d] = tune.kp;
                tune.pi.duty_min[d] = tune.duty_min;
            }
            tune.mode = JOINT_MODE_PI;
        }
        if (have_tune || has_pi)
        {
            (void)joint_set_tune(j, i, tune);
        }
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
