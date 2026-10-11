#include "debug_control.h"
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
    char buf[80]; char *tokens[6]; unsigned n = 0;
    long seq = 0, duty = 0, duration = 0;
    if (strlen(line) >= sizeof(buf)) { answer(c, 0, "ERR", "LINE_TOO_LONG"); return; }
    strcpy(buf, line);
    char *p = strtok(buf, " \t\r\n");
    while (p && n < 6) { tokens[n++] = p; p = strtok(NULL, " \t\r\n"); }
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
    if (n != 5 || strcmp(tokens[0], "RUN") || strlen(tokens[2]) != 2 ||
        tokens[2][0] != 'M' || tokens[2][1] < '1' || tokens[2][1] > '8' ||
        !number(tokens[3], -1000, 1000, &duty) || duty == 0 ||
        !number(tokens[4], 0, 60000, &duration)) {
        answer(c, (unsigned)seq, "ERR", "PARAM"); return;
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
