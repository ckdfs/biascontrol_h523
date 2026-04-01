# Spec 02 — Signal Processing Pipeline

> Status: **In Progress**
> Goal: Pilot tone generation + Goertzel harmonic extraction, verified end-to-end
> Depends on: spec-01-bringup (working DAC + ADC + DMA)

## Current Assessment (2026-04-01)

- DSP core is in place: pilot LUT, H1/H2 Goertzel, CH1 DC accumulation, host-side unit tests, and UART debug commands.
- Current default analysis window is 640 samples (10 coherent pilot cycles, 10 ms).
- Closed-loop and debug paths now both use CH0 for H1/H2 and CH1 for DC.
- Board-side debug is working. On 2026-04-01, `goertzel 3` reported `H1=0.189728 V`, `H2=0.000126 V (-81.0 dBV)`, `DC=0.459994 V`.
- Remaining gaps: documented DAC-to-ADC loopback result, SPI1 DMA write path, and measured ISR timing budget.

## Files to Modify

| File | Action |
|------|--------|
| `dsp/src/dsp_pilot_gen.c` | Implement pilot sine LUT generator |
| `dsp/src/dsp_goertzel.c` | Implement block Goertzel for f0, 2f0 plus DC accumulation |
| `dsp/inc/dsp_types.h` | Define harmonic_data_t if not already |
| `test/test_goertzel.c` | Host-side unit test with synthetic data |
| `cubemx/Core/Src/stm32h5xx_it.c` | Wire DRDY → SPI2 DMA → Goertzel → SPI1 DMA |

## Task Checklist

### 1. Pilot Tone Generator (`dsp_pilot_gen.c`)
- [x] 64-point sine LUT at 1 kHz / 64 kSPS (implemented generically as `sample_rate / frequency`)
- [x] `pilot_gen_init(frequency, sample_rate, amplitude)`: initialize LUT-backed sine source
- [x] `pilot_gen_next()`: return next sample (wraps at LUT end)
- [x] Runtime amplitude is stored as float voltage and injected through `bias_ctrl_get_dac_output()`
- [ ] Verify: oscilloscope on DAC output, clean 1 kHz sine

### 2. Goertzel Algorithm (`dsp_goertzel.c`)
- [x] `goertzel_init(ctx, target_freq, sample_rate, block_size)`
- [x] `goertzel_process_sample(ctx, sample)`: accumulate one sample
- [x] `goertzel_get_magnitude(ctx)`: compute |X[k]| at end of block
- [x] `goertzel_get_phase(ctx)`: compute arg(X[k])
- [x] `goertzel_reset(ctx)`: prepare for next block
- [x] Maintain H1/H2 Goertzel instances plus a dedicated DC accumulator for CH1
- [x] Base pilot period remains 64 samples/cycle at 64 kSPS
- [x] Goertzel analysis block spans multiple complete pilot cycles (default: 10 cycles = 640 samples) to improve weak-tone SNR without spectral leakage

### 3. Host-Side Unit Test (`test/test_goertzel.c`)
- [x] Generate synthetic sine at f0, compute Goertzel, verify magnitude
- [x] Generate sine at f0 + harmonic at 2f0, verify separation
- [x] Verify DC extraction
- [x] Verify phase accuracy (within 1 degree)
- [x] Edge case: all-zero input, DC-only input

### 4. DRDY-Driven Pipeline (ISR integration)
- [x] EXTI11 callback → start SPI2 DMA read
- [x] SPI2 DMA complete → parse ADC data → `goertzel_process_sample()`
- [x] Same callback → compute `dac_output = bias_setpoint + pilot_gen_next()`
- [ ] Start SPI1 DMA write with new DAC value
- [ ] SPI1 DMA complete → LDAC pulse
- [ ] Timing budget: all within ~15.625 µs (1/64kSPS)

Note: the current implementation writes SPI1 in blocking mode from the ADC callback path. This is working for bring-up but is not yet the intended DMA-based final form.

### 5. Loopback Verification
- [ ] Connect DAC output directly to ADC input (wire jumper)
- [ ] Output 1 kHz pilot via DAC, capture via ADC
- [ ] Verify Goertzel f0 magnitude matches expected amplitude
- [ ] Verify 2f0 magnitude is near zero (no distortion)
- [ ] Verify CH1 DC magnitude matches DAC bias offset
- [x] Print harmonics via USART1 for debugging

## Acceptance Criteria
1. Host unit tests pass with <1% magnitude error and <1 degree phase error
   Status: met on 2026-04-01 (`build-test/test_goertzel`, 12 passed / 0 failed).
2. Pilot tone visible on oscilloscope as clean 1 kHz sine
   Status: not yet documented in this spec revision.
3. Loopback Goertzel output matches input within 5%
   Status: not yet documented with dedicated DAC-to-ADC loopback capture.
4. Pipeline runs at 64 kSPS without dropping samples
   Status: partially met in bring-up/debug usage; no formal sample-drop counter has been recorded yet.
5. CPU utilization measured (should be <30% of DRDY period)
   Status: not yet measured.
