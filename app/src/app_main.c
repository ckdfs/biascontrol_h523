#include "app_main.h"
#include "drv_board.h"
#include "drv_dac8568.h"
#include "drv_ads131m02.h"
#include <string.h>

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

    /* Convert raw 24-bit code to voltage */
    float voltage = ads131m02_code_to_voltage(sample->ch0, ADS131M02_GAIN_1);

    /* Feed into bias controller */
    bool ctrl_updated = bias_ctrl_feed_sample(&ctx.bias_ctrl, voltage);

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

    /* Load or set default configuration */
    app_config_load();
    ctx.config = app_config_get();

    transition_to(APP_STATE_HW_SELFTEST);
}

static void state_selftest(void)
{
    /* Initialize DAC8568 */
    int ret = dac8568_init();
    if (ret != 0) {
        transition_to(APP_STATE_FAULT);
        return;
    }

    /* Initialize ADS131M02 */
    ret = ads131m02_init();
    if (ret != 0) {
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

    /* Set DAC outputs to safe state (0V = mid-scale) */
    dac8568_write_channel(DAC8568_CH_ALL, 32768);
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
    /* Update tick (placeholder — will use HAL_GetTick() on hardware) */
    /* ctx.tick_ms = HAL_GetTick(); */

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
        /* TODO: Print state, harmonics, bias voltage via UART */
        /* printf("State: %s\n", app_state_name(ctx.state)); */
        /* printf("Bias: %.3f V\n", bias_ctrl_get_bias_voltage(&ctx.bias_ctrl)); */
        /* printf("Locked: %s\n", bias_ctrl_is_locked(&ctx.bias_ctrl) ? "YES" : "NO"); */
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
