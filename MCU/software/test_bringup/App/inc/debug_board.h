#ifndef DEBUG_BOARD_H
#define DEBUG_BOARD_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { BOARD_LOG, BOARD_LED, BOARD_KEY, BOARD_BUZZER, BOARD_PWR,
               BOARD_ADC, BOARD_LAMP, BOARD_MOTOR } board_kind_t;
typedef enum { BOARD_IDLE, BOARD_SETTLE, BOARD_DRIVE, BOARD_WAIT, BOARD_KEY_WAIT } board_phase_t;
typedef struct {
    bool (*apply)(board_kind_t kind, unsigned index, int value);
    bool (*off)(void);
    void (*reply)(const char *line);
} board_hw_t;
typedef struct {
    board_hw_t hw;
    const char *name;
    board_kind_t kind;
    board_phase_t phase;
    unsigned seq, index, led_step;
    int value;
    uint32_t started_ms, duration_ms;
} debug_board_t;
extern const char *const debug_board_names[29];
void debug_board_init(debug_board_t *b, board_hw_t hw);
const char *debug_board_start(debug_board_t *b, unsigned seq, const char *name,
                            int value, uint32_t duration, uint32_t now);
const char *debug_board_confirm(debug_board_t *b, bool pass);
void debug_board_update(debug_board_t *b, uint32_t now, bool home);
void debug_board_cancel(debug_board_t *b);
const char *debug_board_phase(const debug_board_t *b);
#endif
