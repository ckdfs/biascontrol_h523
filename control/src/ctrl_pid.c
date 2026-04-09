#include "ctrl_pid.h"

static float clampf(float val, float lo, float hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

void pid_init(pid_state_t *pid, float kp, float ki, float dt,
              float out_min, float out_max)
{
    pid->kp = kp;
    pid->ki = ki;
    pid->dt = dt;
    pid->integral = 0.0f;
    pid->out_min = out_min;
    pid->out_max = out_max;
    /* Anti-windup limits default to output limits */
    /*
     * Clamp the integral state so that the integral term alone (ki × integral)
     * stays within [out_min, out_max].  Using the output limits directly as
     * integral limits would allow ki × integral to reach ki × out_max, which
     * can be many times larger than the actual output range when ki > 1.
     * Proper clamping prevents windup that would otherwise take many updates
     * to unwind after the error sign reverses.
     */
    if (ki > 1e-6f) {
        pid->int_min = out_min / ki;
        pid->int_max = out_max / ki;
    } else {
        pid->int_min = out_min;
        pid->int_max = out_max;
    }
}

float pid_update(pid_state_t *pid, float error)
{
    /* Accumulate integral with anti-windup */
    pid->integral += error * pid->dt;
    pid->integral = clampf(pid->integral, pid->int_min, pid->int_max);

    /* PI output */
    float output = pid->kp * error + pid->ki * pid->integral;

    /* Clamp output to DAC/voltage range */
    return clampf(output, pid->out_min, pid->out_max);
}

void pid_reset(pid_state_t *pid)
{
    pid->integral = 0.0f;
}

void pid_set_gains(pid_state_t *pid, float kp, float ki)
{
    pid->kp = kp;
    pid->ki = ki;
}
