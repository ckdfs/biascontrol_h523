#ifndef CTRL_PID_H
#define CTRL_PID_H

/**
 * PI controller with anti-windup clamping.
 *
 * Discrete PI: output = Kp * error + Ki * integral
 * Anti-windup: integral is clamped to [int_min, int_max].
 * Output is clamped to [out_min, out_max].
 */

typedef struct {
    float kp;           /**< Proportional gain */
    float ki;           /**< Integral gain */
    float integral;     /**< Accumulated integral term */
    float int_min;      /**< Integral anti-windup lower limit */
    float int_max;      /**< Integral anti-windup upper limit */
    float out_min;      /**< Output lower clamp */
    float out_max;      /**< Output upper clamp */
    float dt;           /**< Time step in seconds */
} pid_state_t;

/**
 * Initialize the PI controller.
 *
 * @param pid      Controller state
 * @param kp       Proportional gain
 * @param ki       Integral gain
 * @param dt       Control loop period in seconds (e.g., 0.01 for 100Hz)
 * @param out_min  Minimum output value (e.g., -10.0 for -10V)
 * @param out_max  Maximum output value (e.g., +10.0 for +10V)
 */
void pid_init(pid_state_t *pid, float kp, float ki, float dt,
              float out_min, float out_max);

/**
 * Compute one PI control step.
 *
 * @param pid    Controller state
 * @param error  Current error (setpoint - measurement)
 * @return       Control output (clamped)
 */
float pid_update(pid_state_t *pid, float error);

/**
 * Reset the integral accumulator to zero.
 */
void pid_reset(pid_state_t *pid);

/**
 * Set new gains at runtime (e.g., from UART tuning interface).
 */
void pid_set_gains(pid_state_t *pid, float kp, float ki);

#endif /* CTRL_PID_H */
