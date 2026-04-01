/**
 * HAL callback dispatch — overrides HAL weak defaults.
 *
 * HAL declares HAL_GPIO_EXTI_Falling_Callback and HAL_SPI_TxRxCpltCallback
 * as weak symbols.  Defining them here (in a tracked source file) overrides
 * the no-op stubs and routes each event to the appropriate driver.
 *
 * DAC8568 (SPI1) uses blocking HAL_SPI_Transmit — no TX-complete callback.
 * ADS131M02 (SPI2) uses DMA full-duplex — needs TX/RX-complete callback.
 *
 * This file must NOT be placed inside cubemx/ (gitignored).
 */

#include "drv_ads131m02.h"
#include "drv_board.h"
#include "main.h"   /* ADC_DRDY_Pin macro from CubeMX */
#include "spi.h"    /* hspi2 extern declaration */
#include "usart.h"  /* huart1 extern declaration */

/* -------------------------------------------------------------------------
 * EXTI11 (ADC DRDY falling edge) → ADS131M02 DMA read trigger
 * ------------------------------------------------------------------------- */
void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ADC_DRDY_Pin) {
        ads131m02_drdy_isr_handler();
    }
}

/* -------------------------------------------------------------------------
 * SPI2 TX+RX complete (ADS131M02 frame received) → parse and forward
 * ------------------------------------------------------------------------- */
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi2) {
        HAL_SPI_TxRxCpltCallback_ADC(hspi);
    }
}

/* -------------------------------------------------------------------------
 * USART1 TX DMA complete → clear tx-busy flag in app_uart
 * ------------------------------------------------------------------------- */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1) {
        board_uart_tx_cplt();
    }
}
