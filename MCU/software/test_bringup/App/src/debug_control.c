#include "debug_control.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 一行命令最多的空白分隔字段数。 */
#define MAX_TOKENS (24U)

/** @brief 一条回复的最大长度。 */
#define REPLY_MAX (96U)

/** @brief RUN、MSET 的电机持续时间与 TEST 的测试时长上限，单位毫秒。 */
#define MAX_DURATION_MS (60000L)

/** @brief TEST 命令 value 参数的上限（蜂鸣器频率最高 20kHz）。 */
#define MAX_TEST_VALUE (20000L)

/** @brief 关节命令角度参数的绝对值上限，单位毫度。 */
#define MAX_JOINT_VALUE (360000L)

/** @brief JNOM 最后一个参数（电位器阻值，欧姆）的上限；0 表示用标称值。 */
#define MAX_POT_OHMS (1000000L)

/** @brief 关节命令的最大参数个数（JNOM）。 */
#define MAX_JOINT_ARGS (9U)

/** @brief 文本协议能寻址的关节数（M1～M6）。 */
#define JOINT_MOTOR_COUNT (6U)

/** @brief MSET 命令的字段数：命令、序号、掩码，再加每路电机各一个占空比和时长。 */
#define MSET_TOKEN_COUNT (3U + 2U * MOTOR_COUNT)

/** @brief 多路电机掩码：全部电机。 */
#define ALL_MOTORS_MASK ((1L << MOTOR_COUNT) - 1L)

static void answer(debug_control_t *c, unsigned seq, const char *kind,
                   const char *text)
{
    char out[REPLY_MAX];
    (void)snprintf(out, sizeof(out), "%s %u %s\r\n", kind, seq, text);
    c->hw.reply(out);
}
static bool stop(debug_control_t *c)
{
    c->motor = 0;
    c->duty = 0;
    c->pending = false;
    memset(c->multi_duty, 0, sizeof(c->multi_duty));
    memset(c->multi_target, 0, sizeof(c->multi_target));
    c->multi_pending = 0;
    if (!c->hw.stop())
    {
        c->locked = true;
        return false;
    }
    return true;
}
void debug_control_fault(debug_control_t *c, const char *reason)
{
    bool ok = stop(c);
    c->locked = true;
    c->hw.reply(ok ? reason : "EVENT HARDWARE\r\n");
}
void debug_control_init(debug_control_t *c, debug_hw_t hw)
{
    memset(c, 0, sizeof(*c));
    c->hw = hw;
    c->locked = !stop(c);
}
/** @brief 心跳是否有效。 */
static bool has_heartbeat(const debug_control_t *c, uint32_t now)
{
    return c->linked && ((uint32_t)(now - c->heartbeat_ms) <
                         DEBUG_CONTROL_HEARTBEAT_TIMEOUT_MS);
}

/**
 * @brief 解析文本协议里的电机名 "M1"～"M<count>"。
 *
 * @return 0 起的电机下标；格式不对或超出范围时返回 count。
 */
static unsigned parse_motor_name(const char *token, unsigned count)
{
    if ((strlen(token) != 2U) || (token[0] != 'M') || (token[1] < '1') ||
        (token[1] > (char)('0' + count)))
    {
        return count;
    }
    return (unsigned)(token[1] - '1');
}

static bool number(const char *s, long min, long max, long *v)
{
    const char *p = s;
    if (*p == '-')
    {
        ++p;
    }
    if (!*p)
    {
        return false;
    }
    for (; *p; ++p)
    {
        if (!isdigit((unsigned char)*p))
        {
            return false;
        }
    }
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || *end || n < min || n > max)
    {
        return false;
    }
    *v = n;
    return true;
}
void debug_control_line(debug_control_t *c, const char *line, uint32_t now)
{
    char buf[DEBUG_CONTROL_LINE_MAX];
    char *tokens[MAX_TOKENS];
    unsigned n = 0;
    long seq = 0, duty = 0, duration = 0;
    if (strlen(line) >= sizeof(buf))
    {
        answer(c, 0, "ERR", "LINE_TOO_LONG");
        return;
    }
    strcpy(buf, line);
    char *p = strtok(buf, " \t\r\n");
    while (p && n < MAX_TOKENS)
    {
        tokens[n++] = p;
        p = strtok(NULL, " \t\r\n");
    }
    if (p)
    {
        answer(c, 0, "ERR", "FORMAT");
        return;
    }
    if (n < 2 || !number(tokens[1], 0, 65535, &seq))
    {
        answer(c, 0, "ERR", "FORMAT");
        return;
    }
    if (n == 2 && !strcmp(tokens[0], "PING"))
    {
        c->heartbeat_ms = now;
        c->linked = true;
        answer(c, (unsigned)seq, "ACK", "PING");
        return;
    }
    if (n == 3 && !strcmp(tokens[0], "STOP") && !strcmp(tokens[2], "ALL"))
    {
        if (!stop(c))
        {
            answer(c, (unsigned)seq, "ERR", "HARDWARE");
            return;
        }
        /* Explicit STOP clears the stop latch only after HOME is released. */
        if (!c->home)
        {
            c->locked = false;
        }
        answer(c, (unsigned)seq, "ACK", c->locked ? "STOP_LOCKED" : "STOP");
        return;
    }
    if (n == 2 && !strcmp(tokens[0], "INFO"))
    {
        answer(c, (unsigned)seq, "ACK", "INFO");
        if (c->hw.test)
        {
            c->hw.reply("CAPS BOARD_TEST 2\r\n");
        }
        if (c->hw.joint)
        {
            c->hw.reply("CAPS JOINT 1\r\nCAPS JOINT_NOMINAL 1\r\n"
                        "CAPS JOINT_TUNE 1\r\nCAPS TRACE 1\r\n");
        }
        if (c->hw.manual)
        {
            c->hw.reply("CAPS MULTI 1\r\n");
        }
        if (c->hw.aux)
        {
            c->hw.reply("CAPS LIGHT 1\r\nCAPS LINUX 1\r\nCAPS MOTOR_PWM 1\r\n");
        }
        return;
    }
    if (!strcmp(tokens[0], "MSET") || !strcmp(tokens[0], "MOFF") ||
        !strcmp(tokens[0], "LSET") || !strcmp(tokens[0], "LMODE") ||
        !strcmp(tokens[0], "LTX") || !strcmp(tokens[0], "MPWM"))
    {
        bool off = !strcmp(tokens[0], "MOFF");
        if (!off && !has_heartbeat(c, now))
        {
            answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT");
            return;
        }
        if (!off && (c->locked || c->home))
        {
            answer(c, (unsigned)seq, "ERR", "LOCKED");
            return;
        }
        if (off || !strcmp(tokens[0], "MSET"))
        {
            long mask = ALL_MOTORS_MASK;
            long values[MOTOR_COUNT] = {0};
            long times[MOTOR_COUNT] = {0};
            if (!c->hw.manual || (off ? n != 2 : n != MSET_TOKEN_COUNT))
            {
                answer(c, (unsigned)seq, "ERR", "PARAM");
                return;
            }
            if (!off)
            {
                if (!number(tokens[2], 1, ALL_MOTORS_MASK, &mask))
                {
                    answer(c, (unsigned)seq, "ERR", "PARAM");
                    return;
                }
                for (unsigned i = 0; i < MOTOR_COUNT; ++i)
                {
                    if (!number(tokens[i + 3U], -MOTOR_DUTY_SMALL_MAX,
                                MOTOR_DUTY_SMALL_MAX, &values[i]) ||
                        !motor_duty_valid(i, (int)values[i]) ||
                        !number(tokens[i + 3U + MOTOR_COUNT], 0,
                                MAX_DURATION_MS, &times[i]))
                    {
                        answer(c, (unsigned)seq, "ERR", "DUTY_RANGE");
                        return;
                    }
                }
            }
            if (!c->hw.manual())
            {
                debug_control_fault(c, "EVENT HARDWARE\r\n");
                return;
            }
            /* Legacy RUN is exclusive. Manual arrays own outputs from here. */
            if (c->motor || c->pending)
            {
                if (c->motor && !c->hw.run(c->motor - 1U, 0))
                {
                    debug_control_fault(c, "EVENT HARDWARE\r\n");
                    return;
                }
                c->motor = 0;
                c->duty = 0;
                c->pending = false;
            }
            for (unsigned i = 0; i < MOTOR_COUNT; ++i)
            {
                if ((unsigned)mask & (1U << i))
                {
                    if (!c->hw.run(i, 0))
                    {
                        debug_control_fault(c, "EVENT HARDWARE\r\n");
                        return;
                    }
                    c->multi_duty[i] = 0;
                    c->multi_target[i] = (int)values[i];
                    c->multi_duration[i] = (uint32_t)times[i];
                    c->multi_wait[i] = now;
                    c->multi_pending &= ~(1U << i);
                    if (values[i])
                    {
                        c->multi_pending |= 1U << i;
                    }
                }
            }
            answer(c, (unsigned)seq, "ACK", tokens[0]);
            return;
        }
        const char *err =
            c->hw.aux
                ? c->hw.aux(tokens[0], (const char *const *)&tokens[2], n - 2)
                : "UNSUPPORTED";
        answer(c, (unsigned)seq, err ? "ERR" : "ACK", err ? err : tokens[0]);
        return;
    }
    if (!strcmp(tokens[0], "CAL") || !strcmp(tokens[0], "JMOVE") ||
        !strcmp(tokens[0], "JGET") || !strcmp(tokens[0], "JNOM") ||
        !strcmp(tokens[0], "JLIM") || !strcmp(tokens[0], "JTUNE") ||
        !strcmp(tokens[0], "JTRACE"))
    {
        /* JGET、JTRACE 只读：不要求心跳，也不打断正在进行的运动。 */
        bool trace = !strcmp(tokens[0], "JTRACE");
        bool read = !strcmp(tokens[0], "JGET") || trace;
        bool nominal = !strcmp(tokens[0], "JNOM");
        unsigned count = !strcmp(tokens[0], "JGET")    ? 0U
                         : trace                       ? 1U
                         : !strcmp(tokens[0], "CAL")   ? 7U
                         : nominal                     ? MAX_JOINT_ARGS
                         : !strcmp(tokens[0], "JTUNE") ? 4U
                                                       : 2U;
        unsigned joint = (n >= 3U)
                             ? parse_motor_name(tokens[2], JOINT_MOTOR_COUNT)
                             : JOINT_MOTOR_COUNT;
        if (!c->hw.joint || n != count + 3U || joint >= JOINT_MOTOR_COUNT)
        {
            answer(c, (unsigned)seq, "ERR", "JOINT_PARAM");
            return;
        }
        int32_t values[MAX_JOINT_ARGS];
        long value;
        for (unsigned i = 0; i < count; ++i)
        {
            bool is_ohms = nominal && (i == MAX_JOINT_ARGS - 1U);
            if (!number(tokens[i + 3U], is_ohms ? 0L : -MAX_JOINT_VALUE,
                        is_ohms ? MAX_POT_OHMS : MAX_JOINT_VALUE, &value))
            {
                answer(c, (unsigned)seq, "ERR", "JOINT_PARAM");
                return;
            }
            values[i] = (int32_t)value;
        }
        if (!read)
        {
            if (!has_heartbeat(c, now))
            {
                answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT");
                return;
            }
            if (c->locked || c->home)
            {
                answer(c, (unsigned)seq, "ERR", "LOCKED");
                return;
            }
            if (!stop(c))
            {
                answer(c, (unsigned)seq, "ERR", "HARDWARE");
                return;
            }
        }
        const char *err =
            c->hw.joint(tokens[0], (unsigned)seq, joint, values, count, now);
        answer(c, (unsigned)seq, err ? "ERR" : "ACK", err ? err : tokens[0]);
        return;
    }
    if ((n == 3 && !strcmp(tokens[0], "CONFIRM")) ||
        (n == 5 && !strcmp(tokens[0], "TEST")))
    {
        if (!has_heartbeat(c, now))
        {
            answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT");
            return;
        }
        if (c->locked || c->home)
        {
            answer(c, (unsigned)seq, "ERR", "LOCKED");
            return;
        }
        const char *err;
        if (!strcmp(tokens[0], "CONFIRM"))
        {
            if (!c->hw.confirm ||
                (strcmp(tokens[2], "PASS") && strcmp(tokens[2], "FAIL")))
            {
                answer(c, (unsigned)seq, "ERR", "PARAM");
                return;
            }
            err = c->hw.confirm(!strcmp(tokens[2], "PASS"));
        }
        else
        {
            if (!c->hw.test || !number(tokens[3], 0, MAX_TEST_VALUE, &duty) ||
                !number(tokens[4], 1, MAX_DURATION_MS, &duration))
            {
                answer(c, (unsigned)seq, "ERR", "PARAM");
                return;
            }
            if (!stop(c))
            {
                answer(c, (unsigned)seq, "ERR", "HARDWARE");
                return;
            }
            err = c->hw.test((unsigned)seq, tokens[2], (int)duty,
                             (uint32_t)duration);
        }
        answer(c, (unsigned)seq, err ? "ERR" : "ACK", err ? err : tokens[0]);
        return;
    }
    unsigned motor =
        (n == 5) ? parse_motor_name(tokens[2], MOTOR_COUNT) : MOTOR_COUNT;
    if (n != 5 || strcmp(tokens[0], "RUN") || motor >= MOTOR_COUNT ||
        !number(tokens[3], -MOTOR_DUTY_SMALL_MAX, MOTOR_DUTY_SMALL_MAX,
                &duty) ||
        duty == 0 || !number(tokens[4], 0, MAX_DURATION_MS, &duration))
    {
        answer(c, (unsigned)seq, "ERR", "PARAM");
        return;
    }
    if (!motor_duty_valid(motor, (int)duty))
    {
        answer(c, (unsigned)seq, "ERR", "DUTY_RANGE");
        return;
    }
    if (!has_heartbeat(c, now))
    {
        answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT");
        return;
    }
    if (c->locked || c->home)
    {
        answer(c, (unsigned)seq, "ERR", "LOCKED");
        return;
    }
    if (!stop(c))
    {
        answer(c, (unsigned)seq, "ERR", "HARDWARE");
        return;
    }
    c->pending_motor = motor + 1U;
    c->pending_duty = (int)duty;
    c->duration_ms = (uint32_t)duration;
    c->pending_ms = now;
    c->pending = true;
    answer(c, (unsigned)seq, "ACK", "QUEUED");
}
void debug_control_update(debug_control_t *c, uint32_t now, bool home)
{
    bool home_edge = home && !c->home;
    c->home = home;
    if (home_edge)
    {
        debug_control_fault(c, "EVENT HOME\r\n");
    }
    if (c->linked && !has_heartbeat(c, now))
    {
        c->linked = false;
        debug_control_fault(c, "EVENT TIMEOUT\r\n");
    }
    if (!c->locked && !home)
    {
        for (unsigned i = 0; i < MOTOR_COUNT; ++i)
        {
            if ((c->multi_pending & (1U << i)) &&
                (uint32_t)(now - c->multi_wait[i]) >= DEBUG_CONTROL_SETTLE_MS)
            {
                if (!c->hw.run(i, c->multi_target[i]))
                {
                    debug_control_fault(c, "EVENT HARDWARE\r\n");
                    return;
                }
                c->multi_pending &= ~(1U << i);
                c->multi_duty[i] = c->multi_target[i];
                c->multi_start[i] = now;
            }
            if (c->multi_duty[i] && c->multi_duration[i] &&
                (uint32_t)(now - c->multi_start[i]) >= c->multi_duration[i])
            {
                if (!c->hw.run(i, 0))
                {
                    debug_control_fault(c, "EVENT HARDWARE\r\n");
                    return;
                }
                c->multi_duty[i] = c->multi_target[i] = 0;
            }
        }
    }
    if (c->pending && !c->locked && !home &&
        (uint32_t)(now - c->pending_ms) >= DEBUG_CONTROL_SETTLE_MS)
    {
        c->pending = false;
        if (!c->hw.run(c->pending_motor - 1U, c->pending_duty))
        {
            debug_control_fault(c, "EVENT HARDWARE\r\n");
            return;
        }
        c->motor = c->pending_motor;
        c->duty = c->pending_duty;
        c->start_ms = now;
    }
    if (c->motor && c->duration_ms &&
        (uint32_t)(now - c->start_ms) >= c->duration_ms)
    {
        c->hw.reply(stop(c) ? "EVENT DONE\r\n" : "EVENT HARDWARE\r\n");
    }
}
