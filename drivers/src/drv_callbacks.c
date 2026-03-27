/**
 * HAL callback dispatch — overrides HAL weak defaults.
 *
 * HAL declares HAL_GPIO_EXTI_Falling_Callback, HAL_SPI_TxCpltCallback, and
 * HAL_SPI_TxRxCpltCallback as weak symbols.  Defining them here (in a tracked
 * source file) overrides the no-op stubs and routes each event to the
 * appropriate driver.
 *
 * This file must NOT be placed inside cubemx/ (gitignored).  All driver
 * includes must reference project headers, never CubeMX internals directly.
 */

#include "drv_ads131m02.h"
#include "drv_dac8568.h"
#include "main.h"   /* ADC_DRDY_Pin macro from CubeMX */
#include "spi.h"    /* hspi1, hspi2 extern declarations */

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
 * SPI1 TX complete (DAC8568 frame sent) → deassert CS, signal done
 * ------------------------------------------------------------------------- */
void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1) {
        HAL_SPI_TxCpltCallback_DAC(hspi);
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
