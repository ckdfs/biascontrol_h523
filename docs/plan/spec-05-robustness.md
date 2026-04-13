# Spec 05 — Robustness, Tuning & Persistence

> Status: **Pending**
> Goal: Long-term stability verification, runtime PID tuning, parameter persistence, and precision validation
> Depends on: spec-04-mzm-no-dc-5hz (new baseline control path)

## Files to Modify

| File | Action |
|------|--------|
| `app/src/app_main.c` | UART commands for PID tuning, `save` command |
| `app/src/app_config.c` | Flash read/write implementation (`app_config_save/load`) |
| `app/inc/app_config.h` | Flash storage layout, CRC field |
| `control/src/ctrl_bias.c` | Optional streaming output hook |

## Task Checklist

### 1. UART Tuning Interface
- [ ] Runtime PID gain adjustment: `set kp <value>`, `set ki <value>`
- [ ] Runtime lock threshold adjustment: `set lock_threshold <value>`
- [ ] Continuous streaming mode (`stream on|off`) for real-time plotting

### 2. Parameter Persistence
- [ ] Implement `app_config_save()` — write config struct to Flash sector with CRC
- [ ] Implement `app_config_load()` — read and validate from Flash on boot
- [ ] UART command `save` to trigger flash write
- [ ] Retain calibration (Vπ, anchors, harmonic-axis model) across power cycles

### 3. Long-Duration Stability Testing
- [ ] 1-hour lock stability test (all operating points)
- [ ] 24-hour lock stability test (at least QUAD)
- [ ] Log drift rate and maximum excursion

### 4. Disturbance Robustness Testing
- [ ] Vary optical input power (3 dB, 6 dB, 10 dB steps) — verify tracking
- [ ] Temperature cycling (if available) — monitor drift rate
- [ ] Verify re-acquisition after intentional lock-loss

### 5. Precision Verification
- [ ] Measure bias angle accuracy vs MZM transfer curve
- [ ] Target: < 1 degree from ideal bias point
- [ ] Method: compare locked H1/H2 ratio to theoretical expectation
- [ ] Log data for statistical analysis (mean, std, max deviation)

### 6. Acquisition Time Optimization (deferred from spec-03)
- [ ] QUAD acquisition time improvement (currently 146 s, target < 10 s)
- [ ] Profile and optimize calibration sweep + settling sequence

## Acceptance Criteria
1. UART PID tuning responds in < 100 ms
2. Flash storage survives power cycle (config round-trips correctly)
3. Lock maintained across 10 dB optical power variation
4. 1-hour lock stability with < 1 degree drift
5. Bias angle accuracy < 1 degree (measured over 1 hour)
6. QUAD acquisition time < 10 s (stretch goal)
