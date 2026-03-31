#include "app_uart.h"
#include "app_main.h"
#include "usart.h"
#include "stm32h5xx_hal.h"
#include <string.h>

/* =========================================================================
 * USART1 DMA receive — command accumulator
 *
 * STM32H5 GPDMA Ch4 is configured by CubeMX in Linked-List circular mode.
 * HAL_UARTEx_ReceiveToIdle_DMA() kicks off the linked-list DMA once; it
 * then runs FOREVER, filling rx_dma_buf[0..127] repeatedly.
 *
 * The 'Size' argument in HAL_UARTEx_RxEventCallback is the CUMULATIVE byte
 * count since DMA started (not since the last callback).  We track
 * rx_prev_pos to compute the delta and process only the NEW bytes.
 *
 * Circular wrap (Size < rx_prev_pos) is handled explicitly.
 *
 * ISR context isolation:
 *   HAL_UARTEx_RxEventCallback runs in ISR (USART1 or GPDMA1 Ch4 IRQ).
 *   app_handle_command() must NOT be called from ISR — it may block via
 *   board_delay_ms() → HAL_Delay(), which polls SysTick (deadlock in ISR).
 *   ISR only queues the command; app_uart_process() (main loop) dispatches.
 * ========================================================================= */

#define RX_DMA_BUF_SIZE  128u
#define CMD_BUF_SIZE     128u

/* DMA circular buffer — must stay in SRAM accessible by GPDMA1 Ch4 */
static uint8_t rx_dma_buf[RX_DMA_BUF_SIZE];

/* Position of the last byte processed (updated in ISR only) */
static uint16_t rx_prev_pos;

/* Line accumulator (ISR context) */
static char    cmd_buf[CMD_BUF_SIZE];
static uint8_t cmd_len;

/* Command queue: one pending slot is enough for human-typed commands.
 * pending_cmd is written in ISR and read in main loop. */
static char          pending_cmd[CMD_BUF_SIZE];
static volatile bool pending_cmd_ready;

/* -------------------------------------------------------------------------
 * Internal (ISR context): feed one character into the line accumulator.
 * On newline, copy completed command to pending_cmd for main-loop dispatch.
 * ------------------------------------------------------------------------- */
static void process_byte(char c)
{
    if (c == '\r' || c == '\n') {
        if (cmd_len > 0) {
            cmd_buf[cmd_len] = '\0';
            if (!pending_cmd_ready) {
                memcpy(pending_cmd, cmd_buf, cmd_len + 1u);
                pending_cmd_ready = true;
            }
            /* If a command is already pending, drop the new one.
             * For a human-operated interface this is acceptable. */
            cmd_len = 0;
        }
    } else if (cmd_len < CMD_BUF_SIZE - 1u) {
        cmd_buf[cmd_len++] = c;
    }
}

/* -------------------------------------------------------------------------
 * app_uart_init — start GPDMA1 Ch4 linked-list circular reception.
 * Call once from state_init(), after MX_USART1_UART_Init() has run.
 * ------------------------------------------------------------------------- */
void app_uart_init(void)
{
    cmd_len = 0;
    rx_prev_pos = 0;
    pending_cmd_ready = false;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buf, RX_DMA_BUF_SIZE);
}

/* -------------------------------------------------------------------------
 * app_uart_process — dispatch one pending command (main-loop context).
 * Call from app_run() on every iteration.
 * ------------------------------------------------------------------------- */
void app_uart_process(void)
{
    if (!pending_cmd_ready) {
        return;
    }
    char cmd[CMD_BUF_SIZE];
    memcpy(cmd, pending_cmd, CMD_BUF_SIZE);
    pending_cmd_ready = false;
    app_handle_command(cmd);
}

/* -------------------------------------------------------------------------
 * HAL_UARTEx_RxEventCallback — overrides HAL weak default (ISR context)
 *
 * 'Size' is the cumulative byte count in the circular DMA buffer since
 * DMA was started.  We compute new bytes as (Size - rx_prev_pos), handling
 * the circular wrap when Size < rx_prev_pos.
 *
 * Do NOT call HAL_UARTEx_ReceiveToIdle_DMA() here — the linked-list DMA
 * runs continuously without intervention.
 * ------------------------------------------------------------------------- */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart != &huart1 || Size == 0u) {
        return;
    }

    if (Size > rx_prev_pos) {
        /* Common case: no circular wrap */
        for (uint16_t i = rx_prev_pos; i < Size; i++) {
            process_byte((char)rx_dma_buf[i]);
        }
    } else {
        /* Circular wrap: DMA rolled past end of buffer back to position 0 */
        for (uint16_t i = rx_prev_pos; i < RX_DMA_BUF_SIZE; i++) {
            process_byte((char)rx_dma_buf[i]);
        }
        for (uint16_t i = 0u; i < Size; i++) {
            process_byte((char)rx_dma_buf[i]);
        }
    }

    rx_prev_pos = Size;

    /* If DMA completed a full buffer cycle, reset prev_pos for next cycle */
    if (Size == RX_DMA_BUF_SIZE) {
        rx_prev_pos = 0u;
    }
}
