#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H
void debug_console_init(void);
void debug_console_update(void);
void debug_console_rx_from_isr(void);
void debug_console_error_from_isr(void);
void debug_console_linux_rx_from_isr(void);
void debug_console_linux_error_from_isr(void);
#endif
