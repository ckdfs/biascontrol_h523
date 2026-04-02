# Spec 03 — MZM Quadrature Closed-Loop Control

> Status: **In Progress**
> Goal: Lock MZM at quadrature bias point, first working demo
> Depends on: spec-02-dsp-pipeline (working Goertzel + pilot) — **COMPLETE**

## Known Hardware Parameters (from spec-02)

| Parameter | Value | Notes |
|-----------|-------|-------|
| Vπ (VA channel, this MZM) | **5.451 V** | Full-range scan, 4 minima, 3 intervals, 2026-04-02 |
| Vπ repeatability | ±0.065 V (1σ) | Consistent across 4 scan runs |
| Pilot amplitude | 100 mVpp (default) | 0.05 V peak at DAC; ×4 subtractor → 200 mVpp at modulator |
| Scan artifacts | `docs/scans/` | Raw + plots archived |

The quadrature point (H1 maximum, H2 zero-crossing) falls at **Vπ/2 ≈ 2.7 V** from any
transmission minimum. The PID initial acquisition coarse scan can use Vπ = 5.451 V to
set its step size and expected lock range.

## Files to Modify

| File | Action |
|------|--------|
| `control/src/ctrl_pid.c` | Implement PI controller with anti-windup |
| `control/src/ctrl_modulator_mzm.c` | Quad error function |
| `control/src/ctrl_bias.c` | Control loop scheduler (10 Hz, multi-block average) |
| `control/inc/ctrl_modulator.h` | Strategy interface (review/finalize) |
| `app/src/app_main.c` | State machine: INIT→SELFTEST→IDLE→LOCKING→LOCKED |
| `app/src/app_config.c` | Default PID gains, pilot amplitude |
| `test/test_pid.c` | Host-side PID unit test |

## Task Checklist

### 1. PI Controller (`ctrl_pid.c`)
- [ ] `pid_init(ctx, kp, ki, output_min, output_max)`
- [ ] `pid_update(ctx, error, dt)`: returns control output
- [ ] Anti-windup: clamp integrator when output saturates
- [ ] `pid_reset(ctx)`: clear integrator state
- [ ] Host unit test: step response, verify settling time and overshoot

### 2. MZM Quadrature Error (`ctrl_modulator_mzm.c`)
- [ ] Implement `modulator_strategy_t` interface
- [ ] Quad error: `error = H2_magnitude / DC_power`
  - Sign determined by H2 phase (positive = above quad, negative = below)
- [ ] Normalization by DC power → immune to optical power changes
- [ ] `is_locked()`: |error| < threshold for N consecutive cycles
- [ ] `init()`: configure for 1 bias channel, 1 pilot channel

### 3. Control Loop Scheduler (`ctrl_bias.c`)
- [ ] Accumulate Goertzel blocks (current default: 5 blocks × 20 ms → 10 Hz control rate)
- [ ] At each control tick:
  1. Read harmonic_data from Goertzel
  2. Call strategy->compute_error()
  3. Feed error to PID
  4. Update DAC bias setpoint
- [ ] Coarse scan for initial acquisition:
  - Sweep bias from min to max
  - Find zero-crossing of H2 (quadrature point)
  - Switch to PID tracking
- [ ] Output clamping: never exceed DAC range

### 4. State Machine (`app_main.c`)
- [ ] INIT: HAL init, driver init, load config
- [ ] SELFTEST: verify DAC output, ADC reads, LED blink
- [ ] IDLE: wait for start command (UART or auto-start)
- [ ] LOCKING: coarse scan → PID convergence
- [ ] LOCKED: steady-state control, monitor lock quality
- [ ] FAULT: error recovery, re-init

### 5. On-Hardware Verification
- [ ] Connect to real MZM (or MZI with known Vpi)
- [ ] Start with coarse scan, observe bias sweep on oscilloscope
- [ ] Lock at quadrature, verify stable optical output
- [ ] Perturb temperature/bias, verify auto-recovery
- [ ] Log PID error, bias voltage, harmonics via USART1

## Acceptance Criteria
1. PID unit test: step response settles within 50 iterations, <5% overshoot
2. MZM locks at quadrature within 5 seconds from cold start
3. Lock maintained for >1 hour without manual intervention
4. After perturbation, re-locks within 2 seconds
5. Optical power variation at quadrature < 0.5 dB
