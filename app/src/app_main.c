#include "app_main.h"
#include "app_uart.h"
#include "drv_board.h"
#include "drv_dac8568.h"
#include "drv_ads131m02.h"
#include "dsp_goertzel.h"
#include "dsp_types.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* ========================================================================= */
/*  Application context (singleton)                                          */
/* ========================================================================= */

static app_context_t ctx;

/* ========================================================================= */
/*  State name table                                                         */
/* ========================================================================= */

static const char *state_names[] = {
    [APP_STATE_INIT]        = "INIT",
    [APP_STATE_HW_SELFTEST] = "SELFTEST",
    [APP_STATE_IDLE]        = "IDLE",
    [APP_STATE_SWEEPING]    = "SWEEPING",
    [APP_STATE_LOCKING]     = "LOCKING",
    [APP_STATE_LOCKED]      = "LOCKED",
    [APP_STATE_FAULT]       = "FAULT",
};

const char *app_state_name(app_state_t state)
{
    if (state < APP_STATE_COUNT) {
        return state_names[state];
    }
    return "UNKNOWN";
}

/* ========================================================================= */
/*  State transition helper                                                  */
/* ========================================================================= */

static void transition_to(app_state_t new_state)
{
    ctx.state = new_state;
    ctx.state_enter_ms = ctx.tick_ms;
}

/* ========================================================================= */
/*  ADC DRDY callback — feeds samples into bias controller                   */
/* ========================================================================= */

static void adc_drdy_callback(const ads131m02_sample_t *sample)
{
    if (!sample->valid) {
        return;
    }

    /* CH0 carries AC pilot response; CH1 carries DC reference. */
    float ac_voltage = ads131m02_code_to_voltage(sample->ch0, ADS131M02_GAIN_1);
    float dc_voltage = ads131m02_code_to_voltage(sample->ch1, ADS131M02_GAIN_1);

    /* Feed into bias controller */
    bool ctrl_updated = bias_ctrl_feed_sample(&ctx.bias_ctrl, ac_voltage, dc_voltage);

    /* If control loop ran, update DAC output */
    if (ctrl_updated || ctx.bias_ctrl.running) {
        float dac_voltage = bias_ctrl_get_dac_output(&ctx.bias_ctrl);
        dac8568_set_voltage(ctx.config->bias_dac_channel, dac_voltage);
    }
}

/* ========================================================================= */
/*  State handlers                                                           */
/* ========================================================================= */

static void state_init(void)
{
    /* Initialize board hardware */
    board_init();

    /* Start USART1 DMA receive — must come after MX_USART1_UART_Init() */
    app_uart_init();

    /* Load or set default configuration */
    app_config_load();
    ctx.config = app_config_get();

    transition_to(APP_STATE_HW_SELFTEST);
}

static void state_selftest(void)
{
    /* Initialize DAC8568 — non-fatal if chip is not yet soldered */
    int ret = dac8568_init();
    if (ret != 0) {
        printf("[app] WARNING: DAC8568 init failed (%d), continuing without DAC\r\n", ret);
    }

    /* Initialize ADS131M02 — fatal if ADC fails */
    ret = ads131m02_init();
    if (ret != 0) {
        printf("[app] FAULT: ADS131M02 init failed (%d)\r\n", ret);
        transition_to(APP_STATE_FAULT);
        return;
    }

    /* TODO: Selftest — write known DAC value, read back via ADC, verify */

    /* Blink LED to indicate successful init */
    board_led_on();
    board_delay_ms(200);
    board_led_off();

    transition_to(APP_STATE_IDLE);
}

static void state_idle(void)
{
    /* Waiting for user command (via UART) to start bias control.
     * LED slow blink to indicate idle. */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 500) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }
}

static void state_sweeping(void)
{
    /* Perform coarse bias sweep */
    float best_v = bias_ctrl_coarse_sweep(&ctx.bias_ctrl);
    ctx.bias_ctrl.bias_voltage = best_v;

    /* Start closed-loop control */
    bias_ctrl_start(&ctx.bias_ctrl);
    ads131m02_start_continuous(adc_drdy_callback);

    transition_to(APP_STATE_LOCKING);
}

static void state_locking(void)
{
    /* LED fast blink during locking */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 100) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }

    /* Check if locked */
    if (bias_ctrl_is_locked(&ctx.bias_ctrl)) {
        transition_to(APP_STATE_LOCKED);
        return;
    }

    /* Timeout — lock failed */
    if (elapsed > ctx.config->lock_timeout_ms) {
        /* Retry sweep */
        bias_ctrl_stop(&ctx.bias_ctrl);
        ads131m02_stop_continuous();
        transition_to(APP_STATE_SWEEPING);
    }
}

static void state_locked(void)
{
    /* LED solid on when locked */
    board_led_on();

    /* Monitor lock quality */
    if (!bias_ctrl_is_locked(&ctx.bias_ctrl)) {
        /* Lock lost — go back to locking */
        transition_to(APP_STATE_LOCKING);
    }
}

static void state_fault(void)
{
    /* LED rapid blink for fault indication */
    uint32_t elapsed = ctx.tick_ms - ctx.state_enter_ms;
    if ((elapsed / 50) % 2 == 0) {
        board_led_on();
    } else {
        board_led_off();
    }
    /* DAC safe-state write intentionally omitted here —
     * dac8568_write_channel() in a tight fault loop would block
     * if the DMA link is not established. */
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void app_init(void)
{
    memset(&ctx, 0, sizeof(ctx));
    ctx.state = APP_STATE_INIT;
    ctx.tick_ms = 0;
}

void app_run(void)
{
    /* Update tick from HAL systick */
    ctx.tick_ms = HAL_GetTick();

    /* Dispatch any pending UART command (must run in main-loop context, not ISR) */
    app_uart_process();

    switch (ctx.state) {
    case APP_STATE_INIT:
        state_init();
        break;
    case APP_STATE_HW_SELFTEST:
        state_selftest();
        break;
    case APP_STATE_IDLE:
        state_idle();
        break;
    case APP_STATE_SWEEPING:
        state_sweeping();
        break;
    case APP_STATE_LOCKING:
        state_locking();
        break;
    case APP_STATE_LOCKED:
        state_locked();
        break;
    case APP_STATE_FAULT:
        state_fault();
        break;
    default:
        transition_to(APP_STATE_FAULT);
        break;
    }
}

const app_context_t *app_get_context(void)
{
    return &ctx;
}

void app_handle_command(const char *cmd)
{
    if (strcmp(cmd, "start") == 0) {
        if (ctx.state == APP_STATE_IDLE) {
            /* Initialize bias controller with current config */
            bias_ctrl_init(&ctx.bias_ctrl,
                           ctx.config->modulator_type,
                           ctx.config->target_point,
                           ctx.config->initial_bias_v,
                           ctx.config->pilot_amplitude_v,
                           ctx.config->kp,
                           ctx.config->ki);
            transition_to(APP_STATE_SWEEPING);
        }
    } else if (strcmp(cmd, "stop") == 0) {
        bias_ctrl_stop(&ctx.bias_ctrl);
        ads131m02_stop_continuous();
        transition_to(APP_STATE_IDLE);
    } else if (strcmp(cmd, "status") == 0) {
        printf("State: %s\r\n", app_state_name(ctx.state));
        printf("Bias:  %.3f V\r\n", (double)bias_ctrl_get_bias_voltage(&ctx.bias_ctrl));
        printf("Lock:  %s\r\n", bias_ctrl_is_locked(&ctx.bias_ctrl) ? "YES" : "NO");
    } else if (strncmp(cmd, "set bp ", 7) == 0) {
        const char *bp = cmd + 7;
        if (strcmp(bp, "quad") == 0) {
            ctx.config->target_point = BIAS_POINT_QUAD;
        } else if (strcmp(bp, "max") == 0) {
            ctx.config->target_point = BIAS_POINT_MAX;
        } else if (strcmp(bp, "min") == 0) {
            ctx.config->target_point = BIAS_POINT_MIN;
        }
        bias_ctrl_set_target(&ctx.bias_ctrl, ctx.config->target_point);
    } else if (strcmp(cmd, "dac mid") == 0) {
        /* Set all 8 channels to 0 V output (mid-scale code 32768) */
        int ret = dac8568_write_channel(DAC8568_CH_ALL, 32768);
        printf("[dac] all channels -> 0.0 V, ret=%d\r\n", ret);
    } else if (strncmp(cmd, "dac ", 4) == 0) {
        /* "dac <voltage>"  — set DAC channel A to specified voltage (-10..+10 V).
         * Used for hardware verification with a multimeter.
         * Example: "dac 0.0" → subtractor output should read 0.0 V
         *          "dac 5.0" → subtractor output should read 5.0 V  */
        char *endptr = NULL;
        float v = strtof(cmd + 4, &endptr);
        if (endptr != cmd + 4) {
            if (v < -10.0f) v = -10.0f;
            if (v > 10.0f)  v = 10.0f;
            uint16_t code = board_voltage_to_dac_code(v);
            int ret = dac8568_write_channel(DAC8568_CH_A, code);
            if (ret == 0) {
                printf("[dac] CH_A set to %+.3f V (code=%u)\r\n", (double)v, code);
            } else {
                printf("[dac] SPI error %d\r\n", ret);
            }
        } else {
            printf("[dac] usage: dac <voltage>  (e.g. dac 0.0)\r\n");
        }
    } else if (strncmp(cmd, "adc", 3) == 0) {
        /* "adc [N]" — collect N consecutive DRDY-gated samples (default 64),
         * then print them all.
         *
         * Acquisition and printing are intentionally separated: interleaving
         * printf (blocking UART TX) with DRDY polling causes large gaps because
         * each printf line takes ~4 ms at 115200 baud while the ADC fires every
         * 15.6 µs at 64 kSPS, causing ~256 samples to be skipped per print.
         *
         * At 64 kSPS, 64 samples cover 1 ms — one full cycle of a 1 kHz sine. */
        int n = 64;
        if (cmd[3] == ' ' && cmd[4] != '\0') {
            int parsed = atoi(cmd + 4);
            if (parsed > 0 && parsed <= 256) {
                n = parsed;
            }
        }

        /* Phase 1: Acquire all samples back-to-back (no printf here). */
        ads131m02_sample_t samples[256];
        int count = 0;
        bool drdy_timeout = false;

        for (int i = 0; i < n; i++) {
            uint32_t t0 = HAL_GetTick();
            while (board_adc_drdy_read() != 0) {
                if ((HAL_GetTick() - t0) > 5) {
                    drdy_timeout = true;
                    goto adc_collect_done;
                }
            }
            if (ads131m02_read_sample(&samples[count]) == 0) {
                count++;
            }
        }
        adc_collect_done:

        /* Phase 2: Print all collected samples. */
        printf("ADC samples (%d collected", count);
        if (drdy_timeout) {
            printf(", DRDY timeout");
        }
        printf("):\r\n");
        for (int i = 0; i < count; i++) {
            float v0 = ads131m02_code_to_voltage(samples[i].ch0, ADS131M02_GAIN_1);
            float v1 = ads131m02_code_to_voltage(samples[i].ch1, ADS131M02_GAIN_1);
            printf("  [%3d] CH0=%9ld (%+.6fV)  CH1=%9ld (%+.6fV)\r\n",
                   i, (long)samples[i].ch0, (double)v0,
                   (long)samples[i].ch1, (double)v1);
        }
    } else if (strncmp(cmd, "goertzel", 8) == 0) {
        /* "goertzel [N]" — run N Goertzel blocks (default 1), print H1/H2/DC per block.
         * CH0 (AC): extracts f0=1 kHz (H1) and 2f0=2 kHz (H2) via Goertzel.
         * CH1 (DC): block mean via dc_accum.
         * Block size = DSP_GOERTZEL_BLOCK_SIZE (10 full pilot cycles = 10 ms). */
        int n_blocks = 1;
        if (cmd[8] == ' ' && cmd[9] != '\0') {
            int parsed = atoi(cmd + 9);
            if (parsed > 0 && parsed <= 100) {
                n_blocks = parsed;
            }
        }

        goertzel_state_t g_f0, g_2f0;
        dc_accum_t dc;
        goertzel_init(&g_f0,  (float)DSP_PILOT_FREQ_HZ,        (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        goertzel_init(&g_2f0, (float)DSP_PILOT_FREQ_HZ * 2.0f, (float)DSP_SAMPLE_RATE_HZ, DSP_GOERTZEL_BLOCK_SIZE);
        dc_accum_init(&dc, DSP_GOERTZEL_BLOCK_SIZE);

        float h1_sum = 0.0f, h1_sq_sum = 0.0f;
        float h2_sum = 0.0f, h2_sq_sum = 0.0f;
        float dc_sum = 0.0f, dc_sq_sum = 0.0f;
        int blocks_done = 0;

        static const float INV_SQRT2 = 0.70710678f;
        static const float DBV_FLOOR = -120.0f;

        bool drdy_timeout = false;
        for (int b = 0; b < n_blocks && !drdy_timeout; b++) {
            goertzel_reset(&g_f0);
            goertzel_reset(&g_2f0);
            dc_accum_reset(&dc);

            for (uint32_t i = 0; i < DSP_GOERTZEL_BLOCK_SIZE; i++) {
                uint32_t t0 = HAL_GetTick();
                while (board_adc_drdy_read() != 0) {
                    if ((HAL_GetTick() - t0) > 5) {
                        drdy_timeout = true;
                        break;
                    }
                }
                if (drdy_timeout) { break; }

                ads131m02_sample_t s;
                if (ads131m02_read_sample(&s) == 0 && s.valid) {
                    float ch0 = ads131m02_code_to_voltage(s.ch0, ADS131M02_GAIN_1);
                    float ch1 = ads131m02_code_to_voltage(s.ch1, ADS131M02_GAIN_1);
                    goertzel_process_sample(&g_f0,  ch0);
                    goertzel_process_sample(&g_2f0, ch0);
                    dc_accum_process(&dc, ch1);
                }
            }

            if (drdy_timeout) {
                printf("[dsp] DRDY timeout\r\n");
                break;
            }

            float h1_mag, h1_phase_dummy, h2_mag, h2_phase_dummy;
            goertzel_get_result(&g_f0,  &h1_mag, &h1_phase_dummy);
            goertzel_get_result(&g_2f0, &h2_mag, &h2_phase_dummy);
            float dc_val = dc_accum_get_mean(&dc);

            h1_sum += h1_mag;
            h1_sq_sum += h1_mag * h1_mag;
            h2_sum += h2_mag;
            h2_sq_sum += h2_mag * h2_mag;
            dc_sum += dc_val;
            dc_sq_sum += dc_val * dc_val;
            blocks_done++;
        }

        if (blocks_done > 0) {
            float inv_blocks = 1.0f / (float)blocks_done;
            float h1_mean = h1_sum * inv_blocks;
            float h2_mean = h2_sum * inv_blocks;
            float dc_mean = dc_sum * inv_blocks;

            float h1_var = h1_sq_sum * inv_blocks - h1_mean * h1_mean;
            float h2_var = h2_sq_sum * inv_blocks - h2_mean * h2_mean;
            float dc_var = dc_sq_sum * inv_blocks - dc_mean * dc_mean;
            if (h1_var < 0.0f) { h1_var = 0.0f; }
            if (h2_var < 0.0f) { h2_var = 0.0f; }
            if (dc_var < 0.0f) { dc_var = 0.0f; }

            float h1_std = sqrtf(h1_var);
            float h2_std = sqrtf(h2_var);
            float dc_std = sqrtf(dc_var);
            float h1_mean_dbv = (h1_mean > 1e-6f) ? 20.0f * log10f(h1_mean * INV_SQRT2) : DBV_FLOOR;
            float h2_mean_dbv = (h2_mean > 1e-6f) ? 20.0f * log10f(h2_mean * INV_SQRT2) : DBV_FLOOR;
            float h2_rel_dbc = (h1_mean > 1e-9f && h2_mean > 1e-9f) ? 20.0f * log10f(h2_mean / h1_mean) : DBV_FLOOR;
            float window_ms = 1000.0f * (float)(blocks_done * DSP_GOERTZEL_BLOCK_SIZE) / (float)DSP_SAMPLE_RATE_HZ;

            printf("[dsp] mean %d blks (%.1f ms): H1=%+.6fV(%+.1fdBV) sigma=%.4fmV  "
                   "H2=%+.6fV(%+.1fdBV, %+.1fdBc) sigma=%.4fmV  DC=%+.6fV sigma=%.4fmV\r\n",
                   blocks_done, (double)window_ms,
                   (double)h1_mean, (double)h1_mean_dbv, (double)(h1_std * 1000.0f),
                   (double)h2_mean, (double)h2_mean_dbv, (double)h2_rel_dbc, (double)(h2_std * 1000.0f),
                   (double)dc_mean, (double)(dc_std * 1000.0f));
        }
    } else if (strncmp(cmd, "set mod ", 8) == 0) {
        const char *mod = cmd + 8;
        if (strcmp(mod, "mzm") == 0) {
            ctx.config->modulator_type = MOD_TYPE_MZM;
        }
        /* Future: add other modulator types */
        bias_ctrl_set_modulator(&ctx.bias_ctrl, ctx.config->modulator_type);
    }
    /* TODO: Add more commands (set kp, set ki, set pilot, sweep, etc.) */
}
