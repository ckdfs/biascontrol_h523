#include "app_uart.h"
#include "app_main.h"
#include "usart.h"
#include "stm32h5xx_hal.h"

/* =========================================================================
 * USART1 DMA receive — command accumulator
 *
 * Reception model:
 *   HAL_UARTEx_ReceiveToIdle_DMA() arms a single DMA transfer into
 *   rx_dma_buf.  HAL fires HAL_UARTEx_RxEventCallback on two events:
 *     1. UART IDLE line detected after a burst of bytes
 *     2. DMA complete (buffer full)
 *   In both cases we process whatever arrived, then immediately restart.
 *
 * TX (printf) is handled by _write() in drv_board.c via HAL_UART_Transmit.
 * ========================================================================= */

#define RX_DMA_BUF_SIZE  128u   /* Must be large enough for one command line */
#define CMD_BUF_SIZE     128u   /* Parsed command accumulator                */

/* DMA receive buffer — must be in SRAM accessible by GPDMA1 Ch4 */
static uint8_t rx_dma_buf[RX_DMA_BUF_SIZE];

/* Line accumulator for multi-byte commands */
static char    cmd_buf[CMD_BUF_SIZE];
static uint8_t cmd_len;

/* -------------------------------------------------------------------------
 * Internal: append one character to cmd_buf, dispatch on newline
 * ------------------------------------------------------------------------- */
static void process_byte(char c)
{
    if (c == '\r' || c == '\n') {
        if (cmd_len > 0) {
            cmd_buf[cmd_len] = '\0';
            app_handle_command(cmd_buf);
            cmd_len = 0;
        }
    } else if (cmd_len < CMD_BUF_SIZE - 1u) {
        cmd_buf[cmd_len++] = c;
    }
    /* Silently discard when cmd_buf is full — prevents overflow */
}

/* -------------------------------------------------------------------------
 * app_uart_init — arm USART1 DMA reception
 * Call once from state_init() after MX_USART1_UART_Init() has run.
 * ------------------------------------------------------------------------- */
void app_uart_init(void)
{
    cmd_len = 0;
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buf, RX_DMA_BUF_SIZE);
}

/* -------------------------------------------------------------------------
 * HAL_UARTEx_RxEventCallback — overrides HAL weak default
 *
 * Called from USART1_IRQHandler (IDLE) or GPDMA1_Channel4_IRQHandler (TC).
 * huart1.hdmarx is GPDMA1 Ch4 (configured by CubeMX as linked-list).
 * Size = total bytes received into rx_dma_buf since last (re)start.
 * ------------------------------------------------------------------------- */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart != &huart1 || Size == 0u) {
        return;
    }

    for (uint16_t i = 0u; i < Size; i++) {
        process_byte((char)rx_dma_buf[i]);
    }

    /* Restart reception — Size == RX_DMA_BUF_SIZE means buffer was full */
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buf, RX_DMA_BUF_SIZE);
}
