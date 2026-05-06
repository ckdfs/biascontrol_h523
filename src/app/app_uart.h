#ifndef APP_UART_H
#define APP_UART_H

/**
 * USART1 debug/command interface.
 *
 * TX: printf() redirected via _write() syscall → HAL_UART_Transmit (blocking).
 * RX: IDLE + DMA continuous receive → line accumulation → app_handle_command().
 *
 * Protocol: plain ASCII, newline (\n or \r) terminated commands.
 * Baud rate: 115200, 8N1.
 */

/**
 * Start USART1 DMA reception.
 * Call once from state_init(), after MX_USART1_UART_Init() has run.
 */
void app_uart_init(void);

/**
 * Dispatch any pending UART command.
 * Must be called from the main loop (non-ISR context).
 * The UART receive callback only queues commands; this function executes them.
 */
void app_uart_process(void);


#endif /* APP_UART_H */
