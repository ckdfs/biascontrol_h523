#ifndef APP_STATE_H
#define APP_STATE_H

/**
 * Application state machine states.
 *
 * State transitions:
 *
 *   INIT ──► HW_SELFTEST ──► IDLE ──► SWEEPING ──► LOCKING ──► LOCKED
 *              │                                       ▲          │
 *              ▼                                       └──────────┘
 *            FAULT                                    (lock lost)
 *
 * - INIT:        Hardware initialization (clocks, peripherals, drivers)
 * - HW_SELFTEST: Verify DAC/ADC communication, basic signal chain
 * - IDLE:        Waiting for user command to start bias control
 * - SWEEPING:    Coarse bias voltage sweep to find initial operating point
 * - LOCKING:     Closed-loop PID active, attempting to reach target
 * - LOCKED:      Bias point locked, monitoring lock quality
 * - FAULT:       Hardware error detected, outputs set to safe state
 */

typedef enum {
    APP_STATE_INIT,
    APP_STATE_HW_SELFTEST,
    APP_STATE_IDLE,
    APP_STATE_SWEEPING,
    APP_STATE_LOCKING,
    APP_STATE_LOCKED,
    APP_STATE_FAULT,
    APP_STATE_COUNT
} app_state_t;

/** Get human-readable state name */
const char *app_state_name(app_state_t state);

#endif /* APP_STATE_H */
