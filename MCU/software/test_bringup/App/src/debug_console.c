#include "debug_console.h"
#include "debug_control.h"
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
static bool initialized, adc_ok, discard;
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
static bool stop_all(void) { return bsp_motor_stop_all() == BSP_OK; }
static bool run_motor(unsigned motor, int duty)
{
    return bsp_motor_set_duty((bsp_motor_id_t)(motor - 1U), (int16_t)duty) == BSP_OK;
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
    debug_hw_t hw = {stop_all, run_motor, send_line};
    debug_control_init(&control, hw);
    adc_ok = bsp_adc_init() == BSP_OK;
    initialized = true;
    if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1U) != HAL_OK) Error_Handler();
    send_line("READY MOTOR_DEBUG 1\r\n");
}
static void check_control(void)
{
    bool pressed = false; uint32_t count = last_home_count;
    if (bsp_key_read(&pressed) != BSP_OK || bsp_key_get_press_count(&count) != BSP_OK) {
        debug_control_fault(&control, "EVENT KEY_ERROR\r\n"); return;
    }
    /* Count also catches presses during ADC sampling or bounded UART writes. */
    bool edge = count != last_home_count; last_home_count = count;
    debug_control_update(&control, HAL_GetTick(), pressed || edge);
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
        uint16_t pot[6]; char out[128];
        bsp_status_t status = adc_ok ? bsp_adc_read_all(pot, 5U) : BSP_ERR_CONFIG;
        check_control();
        if (status == BSP_OK) {
            (void)snprintf(out, sizeof(out), "POT %lu %u %u %u %u %u %u\r\n",
                (unsigned long)HAL_GetTick(), (unsigned)pot[0], (unsigned)pot[1],
                (unsigned)pot[2], (unsigned)pot[3], (unsigned)pot[4], (unsigned)pot[5]);
        } else (void)snprintf(out, sizeof(out), "ADCERR %lu %d\r\n",
                            (unsigned long)HAL_GetTick(), (int)status);
        send_line(out);
        (void)snprintf(out, sizeof(out), "STATE %lu %u %d %u %u\r\n",
            (unsigned long)HAL_GetTick(), control.motor, control.duty,
            (unsigned)control.pending, (unsigned)control.locked);
        send_line(out);
    }
}
