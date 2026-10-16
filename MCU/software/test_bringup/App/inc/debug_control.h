#ifndef DEBUG_CONTROL_H
#define DEBUG_CONTROL_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool (*stop)(void);
    bool (*run)(unsigned motor, int duty);
    void (*reply)(const char *line);
    const char *(*test)(unsigned seq, const char *name, int value, uint32_t duration);
    const char *(*confirm)(bool pass);
    const char *(*joint)(const char *op, unsigned seq, unsigned id,
                         const int32_t *values, unsigned count, uint32_t now);
    bool (*manual)(void);
    const char *(*aux)(const char *op, const char *const *args, unsigned count);
} debug_hw_t;
typedef struct {
    debug_hw_t hw;
    uint32_t heartbeat_ms, start_ms, duration_ms, pending_ms;
    unsigned motor, pending_motor;
    int duty, pending_duty;
    bool linked, locked, home, pending;
    int multi_duty[8], multi_target[8];
    uint32_t multi_start[8], multi_duration[8], multi_wait[8];
    unsigned multi_pending;
} debug_control_t;
void debug_control_init(debug_control_t *c, debug_hw_t hw);
void debug_control_line(debug_control_t *c, const char *line, uint32_t now);
void debug_control_update(debug_control_t *c, uint32_t now, bool home);
void debug_control_fault(debug_control_t *c, const char *reason);
#endif
