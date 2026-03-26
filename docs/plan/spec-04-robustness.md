# Spec 04 — Multi-Bias-Point & Robustness

> Status: **Pending**
> Goal: Complete MZM demo with max/min points, tuning interface, robustness
> Depends on: spec-03-mzm-quad (working quadrature lock)

## Files to Modify

| File | Action |
|------|--------|
| `control/src/ctrl_modulator_mzm.c` | Add max/min error functions |
| `control/src/ctrl_bias.c` | Lock quality monitor, auto re-acquisition |
| `app/src/app_main.c` | UART command parser for runtime tuning |
| `app/src/app_config.c` | Runtime parameter update |

## Task Checklist

### 1. Max/Min Bias Point Strategies
- [ ] **Max (transmission peak)**:
  - error = H1_magnitude / DC_power
  - Lock condition: H1 ≈ 0, H2 phase < 0
- [ ] **Min (transmission null)**:
  - error = H1_magnitude / DC_power
  - Lock condition: H1 ≈ 0, H2 phase > 0
- [ ] Sign convention: verify with real MZM transfer curve
- [ ] Switch between quad/max/min via UART command

### 2. Lock Quality Monitoring
- [ ] Define lock quality metric: running average of |error|
- [ ] Detect lock loss: error exceeds threshold for N cycles
- [ ] Auto re-acquisition: return to coarse scan on lock loss
- [ ] Hysteresis: don't re-scan on brief transients
- [ ] LED indication: blink=locking, solid=locked, off=fault

### 3. UART Tuning Interface
- [ ] Command format: `SET KP 0.5\r\n`, `GET STATUS\r\n`
- [ ] Adjustable parameters:
  - PID gains (Kp, Ki)
  - Pilot amplitude
  - Target bias point (quad/max/min)
  - Lock threshold
- [ ] Status report: bias voltage, harmonics (H1, H2, DC), lock state, error
- [ ] Continuous streaming mode for real-time plotting

### 4. Robustness Testing
- [ ] Vary optical input power (3 dB, 6 dB, 10 dB steps)
  - Verify bias stays locked (DC normalization works)
- [ ] Vary RF input power (if applicable)
- [ ] Temperature cycling (if available)
  - Monitor bias drift rate, verify tracking
- [ ] Long-duration test: 24-hour lock stability

### 5. Precision Verification
- [ ] Measure bias angle accuracy vs MZM transfer curve
- [ ] Target: < 1 degree from ideal bias point
- [ ] Method: compare locked H1/H2 ratio to theoretical expectation
- [ ] Log data for statistical analysis (mean, std, max deviation)

## Acceptance Criteria
1. Quad/max/min all lock successfully on real MZM
2. Lock maintained across 10 dB optical power variation
3. UART tuning interface responsive (<100 ms round-trip)
4. Auto re-acquisition completes within 5 seconds
5. Bias angle accuracy < 1 degree (measured over 1 hour)
