#include "debug_console.h"
#include "debug_control.h"
#include "debug_board.h"
#include "joint_control.h"
#include "bsp_buzzer.h"
#include "bsp_led.h"
#include "bsp_light.h"
#include "bsp_linux_power.h"
#include "bsp_adc.h"
#include "bsp_key.h"
#include "bsp_motor.h"
#include "main.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

/* ISR produces, main consumes; aligned 32-bit indices are atomic on Cortex-M4. */
static uint8_t rx_byte, ring[256];
static volatile uint32_t head, tail;
static volatile bool rx_fault;
static bool initialized, adc_ok, discard, light_ok, buzzer_ok, power_ok;
static bool home_pressed;
static debug_board_t board;
static joint_control_t joints;
static unsigned joint_focus;
static uint16_t pot_cache[6];
static uint32_t last_adc_ms;
static bool pot_valid;
static char line[80];
static unsigned length;
static uint32_t last_sample, last_home_count;
static debug_control_t control;

static void send_line(const char *text)
{
    /* Bounded transmission: a disconnected host must not stall control. */
    if (HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)strlen(text), 15U) != HAL_OK)
        rx_fault = true;
}
static bool outputs_off(void)
{
    bool ok = bsp_motor_stop_all() == BSP_OK;
    for (unsigned i = 0; i < BSP_LED_ID_COUNT; ++i)
        if (bsp_led_set((bsp_led_id_t)i, false) != BSP_OK) ok = false;
    if (light_ok) for (unsigned i = 0; i < BSP_LIGHT_COUNT; ++i)
        if (bsp_light_stop((bsp_light_id_t)i) != BSP_OK) ok = false;
    if (buzzer_ok && bsp_buzzer_stop() != BSP_OK) ok = false;
    if (power_ok && bsp_linux_power_release() != BSP_OK) ok = false;
    return ok;
}
static bool stop_all(void)
{
    bool ok = joint_cancel(&joints);
    debug_board_cancel(&board);
    return outputs_off() && ok;
}
static bool board_apply(board_kind_t kind, unsigned index, int value)
{
    switch (kind) {
        case BOARD_LOG:
            send_line("LOGTEST BRINGUP log ok\r\n"); return !rx_fault;
        case BOARD_LED: return bsp_led_set((bsp_led_id_t)index, true) == BSP_OK;
        case BOARD_KEY: return true;
        case BOARD_BUZZER: return buzzer_ok && bsp_buzzer_start((uint32_t)value) == BSP_OK;
        case BOARD_PWR: return power_ok && bsp_linux_power_start(board.duration_ms) == BSP_OK;
        case BOARD_ADC: {
            uint16_t values[6];
            return adc_ok && bsp_adc_read_all(values, 5U) == BSP_OK;
        }
        case BOARD_LAMP:
            return light_ok && bsp_light_set_duty((bsp_light_id_t)index, (uint16_t)value) == BSP_OK;
        case BOARD_MOTOR:
            return bsp_motor_set_duty((bsp_motor_id_t)index, (int16_t)value) == BSP_OK;
        default: return false;
    }
}
static const char *start_test(unsigned seq, const char *name, int value, uint32_t duration)
{
    return debug_board_start(&board, seq, name, value, duration, HAL_GetTick());
}
static const char *confirm_test(bool pass) { return debug_board_confirm(&board, pass); }
static bool run_motor(unsigned motor, int duty)
{
    return bsp_motor_set_duty((bsp_motor_id_t)(motor - 1U), (int16_t)duty) == BSP_OK;
}
static bool joint_drive(unsigned index, int duty)
{
    return bsp_motor_set_duty((bsp_motor_id_t)index, (int16_t)duty) == BSP_OK;
}
static void joint_report(uint32_t now, bool config)
{
    joint_state_t *s = &joints.states[joint_focus];
    char out[128];
    if (config && s->calibrated) {
        (void)snprintf(out, sizeof(out), "JCAL %lu M%u %u %ld %u %ld %d %u %u\r\n",
            (unsigned long)now, joint_focus + 1U, (unsigned)s->cal.raw_a, (long)s->cal.angle_a,
            (unsigned)s->cal.raw_b, (long)s->cal.angle_b, s->cal.direction, s->cal.duty, s->cal.tolerance);
        send_line(out);
    }
    (void)snprintf(out, sizeof(out), "JSTATE %lu M%u %u %u %u %ld %ld %d %s %s\r\n",
        (unsigned long)now, joint_focus + 1U, (unsigned)s->calibrated,
        (unsigned)(joints.valid && (uint32_t)(HAL_GetTick() - joints.sample_ms) <= 50U),
        (unsigned)s->raw, (long)s->angle, (long)s->target, s->duty,
        joint_phase_name(s->phase), s->fault);
    send_line(out);
}
static const char *joint_command(const char *op, unsigned seq, unsigned index,
                                  const int32_t *v, unsigned count, uint32_t now)
{
    joint_focus = index;
    if (!strcmp(op, "JGET")) { joint_report(now, true); return NULL; }
    if (!strcmp(op, "CAL") && count == 7U) {
        if (v[0] < 1 || v[0] > 4094 || v[2] < 1 || v[2] > 4094 || v[5] < 1 || v[6] < 0)
            return "CAL_PARAM";
        joint_cal_t cal = {(uint16_t)v[0], (uint16_t)v[2], v[1], v[3], (int)v[4],
                            (unsigned)v[5], (unsigned)v[6]};
        const char *err = joint_calibrate(&joints, index, cal);
        if (!err) joint_report(now, true);
        return err;
    }
    if (!strcmp(op, "JMOVE") && count == 2U)
        return joint_move(&joints, seq, index, v[0], (uint32_t)v[1], now);
    return "JOINT_PARAM";
}
void debug_console_rx_from_isr(void)
{
    if (!initialized) return;
    uint32_t next = (head + 1U) & 255U;
    if (next == tail) rx_fault = true;
    else { ring[head] = rx_byte; __DMB(); head = next; }
    if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) rx_fault = true;
}
void debug_console_error_from_isr(void) { if (initialized) rx_fault = true; }
void debug_console_init(void)
{
    if (bsp_motor_init() != BSP_OK || bsp_key_init() != BSP_OK) Error_Handler();
    light_ok = bsp_light_init() == BSP_OK;
    buzzer_ok = bsp_buzzer_init() == BSP_OK;
    power_ok = bsp_linux_power_init() == BSP_OK;
    board_hw_t board_hw = {board_apply, outputs_off, send_line};
    debug_board_init(&board, board_hw);
    joint_hw_t joint_hw = {joint_drive, send_line};
    joint_init(&joints, joint_hw);
    debug_hw_t hw = {stop_all, run_motor, send_line, start_test, confirm_test, joint_command};
    debug_control_init(&control, hw);
    adc_ok = bsp_adc_init() == BSP_OK;
    initialized = true;
    if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) Error_Handler();
    send_line("READY BOARD_DEBUG 2\r\n");
}
static void check_control(void)
{
    bool pressed = false; uint32_t count = last_home_count;
    if (bsp_key_read(&pressed) != BSP_OK || bsp_key_get_press_count(&count) != BSP_OK) {
        debug_control_fault(&control, "EVENT KEY_ERROR\r\n"); return;
    }
    /* Count also catches presses during ADC sampling or bounded UART writes. */
    bool edge = count != last_home_count; last_home_count = count;
    home_pressed = pressed;
    uint32_t now = HAL_GetTick();
    /* KEY reports the real press before the common HOME stop latch is applied. */
    if (board.phase == BOARD_KEY_WAIT) debug_board_update(&board, now, pressed || edge);
    debug_control_update(&control, now, pressed || edge);
    if (power_ok && bsp_linux_power_update() != BSP_OK)
        debug_control_fault(&control, "EVENT POWER_ERROR\r\n");
    if (!control.locked) debug_board_update(&board, HAL_GetTick(), false);
    now = HAL_GetTick();
    if ((uint32_t)(now - last_adc_ms) >= 10U) {
        last_adc_ms = now;
        pot_valid = adc_ok && bsp_adc_read_all(pot_cache, 5U) == BSP_OK;
        joint_feed(&joints, pot_cache, pot_valid, HAL_GetTick());
    }
    if (!joint_tick(&joints, HAL_GetTick()))
        debug_control_fault(&control, "EVENT JOINT_FAULT\r\n");
}
void debug_console_update(void)
{
    check_control();
    if (rx_fault) {
        (void)HAL_UART_AbortReceive(&huart3);
        tail = head; length = 0; discard = true; rx_fault = false;
        debug_control_fault(&control, "EVENT UART_ERROR\r\n");
        if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) Error_Handler();
    }
    unsigned budget = 128U;
    while (tail != head && budget--) {
        char ch = (char)ring[tail]; __DMB(); tail = (tail + 1U) & 255U;
        if (ch == '\r') continue;
        if (ch == '\n') {
            if (!discard && length) {
                line[length] = 0; check_control();
                debug_control_line(&control, line, HAL_GetTick());
            }
            length = 0; discard = false;
        } else if (!discard) {
            if (ch < 32 || ch > 126 || length >= sizeof(line) - 1U) {
                discard = true; length = 0;
                debug_control_fault(&control, "EVENT BAD_LINE\r\n");
            } else line[length++] = ch;
        }
        if (rx_fault) break;
    }
    check_control();
    uint32_t now = HAL_GetTick();
    if ((uint32_t)(now - last_sample) >= 100U) {
        last_sample = now;
        char out[128];
        check_control();
        if (pot_valid) {
            (void)snprintf(out, sizeof(out), "POT %lu %u %u %u %u %u %u\r\n",
                (unsigned long)HAL_GetTick(), (unsigned)pot_cache[0], (unsigned)pot_cache[1],
                (unsigned)pot_cache[2], (unsigned)pot_cache[3], (unsigned)pot_cache[4], (unsigned)pot_cache[5]);
        } else (void)snprintf(out, sizeof(out), "ADCERR %lu %d\r\n",
                            (unsigned long)HAL_GetTick(), (int)BSP_ERR_HAL);
        send_line(out);
        check_control();
        unsigned active_motor = control.motor;
        int active_duty = control.duty;
        bool pending_motor = control.pending;
        if (board.kind == BOARD_MOTOR && board.phase == BOARD_DRIVE) {
            active_motor = board.index + 1U; active_duty = board.value;
        }
        if (board.kind == BOARD_MOTOR && board.phase == BOARD_SETTLE) pending_motor = true;
        if (joints.active >= 0) {
            active_motor = (unsigned)joints.active + 1U;
            active_duty = joints.states[joints.active].duty;
            pending_motor = joints.states[joints.active].phase == JOINT_SETTLE;
        }
        (void)snprintf(out, sizeof(out), "STATE %lu %u %d %u %u\r\n",
            (unsigned long)HAL_GetTick(), active_motor, active_duty,
            (unsigned)pending_motor, (unsigned)control.locked);
        send_line(out);
        check_control();
        (void)snprintf(out, sizeof(out), "BOARD %lu %s %s\r\n",
            (unsigned long)HAL_GetTick(), board.phase == BOARD_IDLE ? "-" : board.name,
            debug_board_phase(&board));
        send_line(out);
        check_control();
        (void)snprintf(out, sizeof(out), "KEY %lu %u %lu\r\n",
            (unsigned long)HAL_GetTick(), (unsigned)home_pressed, (unsigned long)last_home_count);
        send_line(out);
        check_control();
        joint_report(HAL_GetTick(), false);
    }
}
