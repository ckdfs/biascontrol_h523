#include "app_config.h"

static app_config_t config;

void app_config_defaults(void)
{
    config.modulator_type = MOD_TYPE_MZM;
    config.target_point = BIAS_POINT_QUAD;
    config.kp = 1.0f;
    config.ki = 10.0f;
    config.initial_bias_v = 0.0f;
    config.pilot_amplitude_v = 0.05f;   /* 50 mV */
    config.pilot_freq_hz = 1000.0f;     /* 1 kHz */
    config.bias_dac_channel = 0;        /* Channel A (VA) */
    config.lock_threshold = 0.02f;
    config.lock_timeout_ms = 5000;
}

app_config_t *app_config_get(void)
{
    return &config;
}

int app_config_save(void)
{
    /* TODO: Save to Flash sector */
    return -1;
}

int app_config_load(void)
{
    /* TODO: Load from Flash, validate CRC */
    /* For now, use defaults */
    app_config_defaults();
    return -1;
}
