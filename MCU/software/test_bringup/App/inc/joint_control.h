#ifndef JOINT_CONTROL_H
#define JOINT_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
#define JOINT_COUNT 6U
typedef struct {
    uint16_t raw_a, raw_b;
    int32_t angle_a, angle_b; /* millidegrees, ascending angles; ADC may descend */
    int direction; /* motor FI increases angle: +1; decreases: -1 */
    unsigned duty, tolerance;
} joint_cal_t;
typedef enum { JOINT_IDLE, JOINT_SETTLE, JOINT_MOVING, JOINT_DONE, JOINT_FAULT } joint_phase_t;
typedef struct {
    joint_cal_t cal;
    bool calibrated;
    uint16_t raw;
    int32_t angle, target;
    int duty;
    joint_phase_t phase;
    const char *fault;
} joint_state_t;
typedef struct {
    bool (*drive)(unsigned index, int duty);
    void (*reply)(const char *line);
} joint_hw_t;
typedef struct {
    joint_hw_t hw;
    joint_state_t states[JOINT_COUNT];
    int active, last_sign;
    unsigned seq;
    uint32_t sample_ms, start_ms, progress_ms, stable_ms, switch_ms, pulse_ms, timeout_ms;
    int32_t initial_error, best_error;
    bool valid, stable, pulse_on, near_mode;
} joint_control_t;
void joint_init(joint_control_t *j, joint_hw_t hw);
bool joint_cal_valid(const joint_cal_t *c);
int32_t joint_angle(const joint_cal_t *c, uint16_t raw);
const char *joint_calibrate(joint_control_t *j, unsigned index, joint_cal_t c);
const char *joint_move(joint_control_t *j, unsigned seq, unsigned index, int32_t target,
                       uint32_t timeout, uint32_t now);
void joint_feed(joint_control_t *j, const uint16_t *raw, bool valid, uint32_t now);
bool joint_tick(joint_control_t *j, uint32_t now); /* false on a new control fault */
bool joint_cancel(joint_control_t *j);
const char *joint_phase_name(joint_phase_t phase);
#endif
