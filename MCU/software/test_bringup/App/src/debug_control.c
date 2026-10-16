#include "debug_control.h"
#include "motor_limits.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void answer(debug_control_t *c, unsigned seq, const char *kind, const char *text)
{
    char out[96];
    (void)snprintf(out, sizeof(out), "%s %u %s\r\n", kind, seq, text);
    c->hw.reply(out);
}
static bool stop(debug_control_t *c)
{
    c->motor = 0; c->duty = 0; c->pending = false;
    memset(c->multi_duty, 0, sizeof(c->multi_duty));
    memset(c->multi_target, 0, sizeof(c->multi_target)); c->multi_pending = 0;
    if (!c->hw.stop()) { c->locked = true; return false; }
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
    memset(c, 0, sizeof(*c)); c->hw = hw;
    c->locked = !stop(c);
}
static bool number(const char *s, long min, long max, long *v)
{
    const char *p = s;
    if (*p == '-') ++p;
    if (!*p) return false;
    for (; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    char *end;
    errno = 0; long n = strtol(s, &end, 10);
    if (errno || *end || n < min || n > max) return false;
    *v = n; return true;
}
void debug_control_line(debug_control_t *c, const char *line, uint32_t now)
{
    char buf[192]; char *tokens[24]; unsigned n = 0;
    long seq = 0, duty = 0, duration = 0;
    if (strlen(line) >= sizeof(buf)) { answer(c, 0, "ERR", "LINE_TOO_LONG"); return; }
    strcpy(buf, line);
    char *p = strtok(buf, " \t\r\n");
    while (p && n < 24) { tokens[n++] = p; p = strtok(NULL, " \t\r\n"); }
    if (p) { answer(c, 0, "ERR", "FORMAT"); return; }
    if (n < 2 || !number(tokens[1], 0, 65535, &seq)) {
        answer(c, 0, "ERR", "FORMAT"); return;
    }
    if (n == 2 && !strcmp(tokens[0], "PING")) {
        c->heartbeat_ms = now; c->linked = true;
        answer(c, (unsigned)seq, "ACK", "PING"); return;
    }
    if (n == 3 && !strcmp(tokens[0], "STOP") && !strcmp(tokens[2], "ALL")) {
        if (!stop(c)) { answer(c, (unsigned)seq, "ERR", "HARDWARE"); return; }
        /* Explicit STOP clears the stop latch only after HOME is released. */
        if (!c->home) c->locked = false;
        answer(c, (unsigned)seq, "ACK", c->locked ? "STOP_LOCKED" : "STOP"); return;
    }
    if (n == 2 && !strcmp(tokens[0], "INFO")) {
        answer(c, (unsigned)seq, "ACK", "INFO");
        if (c->hw.test) c->hw.reply("CAPS BOARD_TEST 2\r\n");
        if (c->hw.joint) c->hw.reply("CAPS JOINT 1\r\n");
        if (c->hw.manual) c->hw.reply("CAPS MULTI 1\r\n");
        if (c->hw.aux) c->hw.reply("CAPS LIGHT 1\r\nCAPS LINUX 1\r\n");
        return;
    }
    if (!strcmp(tokens[0], "MSET") || !strcmp(tokens[0], "MOFF") ||
        !strcmp(tokens[0], "LSET") || !strcmp(tokens[0], "LMODE") || !strcmp(tokens[0], "LTX")) {
        bool off = !strcmp(tokens[0], "MOFF");
        if (!off && (!c->linked || (uint32_t)(now - c->heartbeat_ms) >= 1000U)) {
            answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT"); return;
        }
        if (!off && (c->locked || c->home)) { answer(c, (unsigned)seq, "ERR", "LOCKED"); return; }
        if (off || !strcmp(tokens[0], "MSET")) {
            long mask = 255, values[8] = {0}, times[8] = {0};
            if (!c->hw.manual || (off ? n != 2 : n != 19)) {
                answer(c, (unsigned)seq, "ERR", "PARAM"); return;
            }
            if (!off) {
                if (!number(tokens[2], 1, 255, &mask)) {
                    answer(c, (unsigned)seq, "ERR", "PARAM"); return;
                }
                for (unsigned i = 0; i < 8; ++i) {
                    if (!number(tokens[i+3], -1000, 1000, &values[i]) ||
                        !motor_duty_valid(i+1, (int)values[i]) || !number(tokens[i+11], 0, 60000, &times[i])) {
                        answer(c, (unsigned)seq, "ERR", "DUTY_RANGE"); return;
                    }
                }
            }
            if (!c->hw.manual()) { debug_control_fault(c, "EVENT HARDWARE\r\n"); return; }
            /* Legacy RUN is exclusive. Manual arrays own outputs from here. */
            if (c->motor || c->pending) {
                if (c->motor && !c->hw.run(c->motor, 0)) { debug_control_fault(c, "EVENT HARDWARE\r\n"); return; }
                c->motor = 0; c->duty = 0; c->pending = false;
            }
            for (unsigned i = 0; i < 8; ++i) if ((unsigned)mask & (1U << i)) {
                if (!c->hw.run(i+1, 0)) { debug_control_fault(c, "EVENT HARDWARE\r\n"); return; }
                c->multi_duty[i] = 0; c->multi_target[i] = (int)values[i];
                c->multi_duration[i] = (uint32_t)times[i]; c->multi_wait[i] = now;
                c->multi_pending &= ~(1U << i);
                if (values[i]) c->multi_pending |= 1U << i;
            }
            answer(c, (unsigned)seq, "ACK", tokens[0]); return;
        }
        const char *err = c->hw.aux ? c->hw.aux(tokens[0], (const char *const *)&tokens[2], n-2) : "UNSUPPORTED";
        answer(c, (unsigned)seq, err ? "ERR" : "ACK", err ? err : tokens[0]); return;
    }
    if (!strcmp(tokens[0], "CAL") || !strcmp(tokens[0], "JMOVE") || !strcmp(tokens[0], "JGET")) {
        bool read = !strcmp(tokens[0], "JGET");
        unsigned count = read ? 0U : !strcmp(tokens[0], "CAL") ? 7U : 2U;
        if (!c->hw.joint || n != count + 3U || strlen(tokens[2]) != 2 ||
            tokens[2][0] != 'M' || tokens[2][1] < '1' || tokens[2][1] > '6') {
            answer(c, (unsigned)seq, "ERR", "JOINT_PARAM"); return;
        }
        int32_t values[7]; long value;
        for (unsigned i = 0; i < count; ++i) {
            if (!number(tokens[i + 3U], -360000, 360000, &value)) {
                answer(c, (unsigned)seq, "ERR", "JOINT_PARAM"); return;
            }
            values[i] = (int32_t)value;
        }
        if (!read) {
            if (!c->linked || (uint32_t)(now - c->heartbeat_ms) >= 1000U) {
                answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT"); return;
            }
            if (c->locked || c->home) { answer(c, (unsigned)seq, "ERR", "LOCKED"); return; }
            if (!stop(c)) { answer(c, (unsigned)seq, "ERR", "HARDWARE"); return; }
        }
        const char *err = c->hw.joint(tokens[0], (unsigned)seq, (unsigned)(tokens[2][1] - '1'),
                                     values, count, now);
        answer(c, (unsigned)seq, err ? "ERR" : "ACK", err ? err : tokens[0]); return;
    }
    if ((n == 3 && !strcmp(tokens[0], "CONFIRM")) ||
        (n == 5 && !strcmp(tokens[0], "TEST"))) {
        if (!c->linked || (uint32_t)(now - c->heartbeat_ms) >= 1000U) {
            answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT"); return;
        }
        if (c->locked || c->home) { answer(c, (unsigned)seq, "ERR", "LOCKED"); return; }
        const char *err;
        if (!strcmp(tokens[0], "CONFIRM")) {
            if (!c->hw.confirm || (strcmp(tokens[2], "PASS") && strcmp(tokens[2], "FAIL"))) {
                answer(c, (unsigned)seq, "ERR", "PARAM"); return;
            }
            err = c->hw.confirm(!strcmp(tokens[2], "PASS"));
        } else {
            if (!c->hw.test || !number(tokens[3], 0, 20000, &duty) ||
                !number(tokens[4], 1, 60000, &duration)) {
                answer(c, (unsigned)seq, "ERR", "PARAM"); return;
            }
            if (!stop(c)) { answer(c, (unsigned)seq, "ERR", "HARDWARE"); return; }
            err = c->hw.test((unsigned)seq, tokens[2], (int)duty, (uint32_t)duration);
        }
        answer(c, (unsigned)seq, err ? "ERR" : "ACK", err ? err : tokens[0]); return;
    }
    if (n != 5 || strcmp(tokens[0], "RUN") || strlen(tokens[2]) != 2 ||
        tokens[2][0] != 'M' || tokens[2][1] < '1' || tokens[2][1] > '8' ||
        !number(tokens[3], -1000, 1000, &duty) || duty == 0 ||
        !number(tokens[4], 0, 60000, &duration)) {
        answer(c, (unsigned)seq, "ERR", "PARAM"); return;
    }
    if (!motor_duty_valid((unsigned)(tokens[2][1] - '0'), (int)duty)) {
        answer(c, (unsigned)seq, "ERR", "DUTY_RANGE"); return;
    }
    if (!c->linked || (uint32_t)(now - c->heartbeat_ms) >= 1000U) {
        answer(c, (unsigned)seq, "ERR", "NO_HEARTBEAT"); return;
    }
    if (c->locked || c->home) { answer(c, (unsigned)seq, "ERR", "LOCKED"); return; }
    if (!stop(c)) { answer(c, (unsigned)seq, "ERR", "HARDWARE"); return; }
    c->pending_motor = (unsigned)(tokens[2][1] - '0');
    c->pending_duty = (int)duty; c->duration_ms = (uint32_t)duration;
    c->pending_ms = now; c->pending = true;
    answer(c, (unsigned)seq, "ACK", "QUEUED");
}
void debug_control_update(debug_control_t *c, uint32_t now, bool home)
{
    bool home_edge = home && !c->home;
    c->home = home;
    if (home_edge) debug_control_fault(c, "EVENT HOME\r\n");
    if (c->linked && (uint32_t)(now - c->heartbeat_ms) >= 1000U) {
        c->linked = false; debug_control_fault(c, "EVENT TIMEOUT\r\n");
    }
    if (!c->locked && !home) for (unsigned i = 0; i < 8; ++i) {
        if ((c->multi_pending & (1U << i)) && (uint32_t)(now - c->multi_wait[i]) >= 100U) {
            if (!c->hw.run(i+1, c->multi_target[i])) { debug_control_fault(c, "EVENT HARDWARE\r\n"); return; }
            c->multi_pending &= ~(1U << i); c->multi_duty[i] = c->multi_target[i]; c->multi_start[i] = now;
        }
        if (c->multi_duty[i] && c->multi_duration[i] &&
            (uint32_t)(now - c->multi_start[i]) >= c->multi_duration[i]) {
            if (!c->hw.run(i+1, 0)) { debug_control_fault(c, "EVENT HARDWARE\r\n"); return; }
            c->multi_duty[i] = c->multi_target[i] = 0;
        }
    }
    if (c->pending && !c->locked && !home && (uint32_t)(now - c->pending_ms) >= 100U) {
        c->pending = false;
        if (!c->hw.run(c->pending_motor, c->pending_duty)) {
            debug_control_fault(c, "EVENT HARDWARE\r\n"); return;
        }
        c->motor = c->pending_motor; c->duty = c->pending_duty; c->start_ms = now;
    }
    if (c->motor && c->duration_ms && (uint32_t)(now - c->start_ms) >= c->duration_ms) {
        c->hw.reply(stop(c) ? "EVENT DONE\r\n" : "EVENT HARDWARE\r\n");
    }
}
