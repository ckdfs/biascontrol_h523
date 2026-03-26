# Spec 02 — Signal Processing Pipeline

> Status: **Pending**
> Goal: Pilot tone generation + Goertzel harmonic extraction, verified end-to-end
> Depends on: spec-01-bringup (working DAC + ADC + DMA)

## Files to Modify

| File | Action |
|------|--------|
| `dsp/src/dsp_pilot_gen.c` | Implement 32-point sine LUT generator |
| `dsp/src/dsp_goertzel.c` | Implement sliding Goertzel for f0, 2f0, DC |
| `dsp/inc/dsp_types.h` | Define harmonic_data_t if not already |
| `test/test_goertzel.c` | Host-side unit test with synthetic data |
| `cubemx/Core/Src/stm32h5xx_it.c` | Wire DRDY → SPI2 DMA → Goertzel → SPI1 DMA |

## Task Checklist

### 1. Pilot Tone Generator (`dsp_pilot_gen.c`)
- [ ] 32-point sine LUT (1 kHz @ 32 kSPS → exactly 32 samples/period)
- [ ] `pilot_gen_init(amplitude_lsb)`: set amplitude in DAC LSB units
- [ ] `pilot_gen_next()`: return next sample (wraps at index 32)
- [ ] Amplitude: ~164 LSB → ~50 mV after subtractor (GAIN=4, DAC 5V/65536)
- [ ] Verify: oscilloscope on DAC output, clean 1 kHz sine

### 2. Goertzel Algorithm (`dsp_goertzel.c`)
- [ ] `goertzel_init(ctx, target_freq, sample_rate, block_size)`
- [ ] `goertzel_process_sample(ctx, sample)`: accumulate one sample
- [ ] `goertzel_get_magnitude(ctx)`: compute |X[k]| at end of block
- [ ] `goertzel_get_phase(ctx)`: compute arg(X[k])
- [ ] `goertzel_reset(ctx)`: prepare for next block
- [ ] Maintain 3 instances: DC (k=0), f0 (k=1), 2f0 (k=2)
- [ ] Block size N=32 ensures integer cycles (no spectral leakage)

### 3. Host-Side Unit Test (`test/test_goertzel.c`)
- [ ] Generate synthetic sine at f0, compute Goertzel, verify magnitude
- [ ] Generate sine at f0 + harmonic at 2f0, verify separation
- [ ] Verify DC extraction
- [ ] Verify phase accuracy (within 1 degree)
- [ ] Edge case: all-zero input, DC-only input

### 4. DRDY-Driven Pipeline (ISR integration)
- [ ] EXTI11 callback → start SPI2 DMA read
- [ ] SPI2 DMA complete → parse ADC data → `goertzel_process_sample()`
- [ ] Same callback → compute `dac_output = bias_setpoint + pilot_gen_next()`
- [ ] Start SPI1 DMA write with new DAC value
- [ ] SPI1 DMA complete → LDAC pulse
- [ ] Timing budget: all within ~31.25 us (1/32kSPS)

### 5. Loopback Verification
- [ ] Connect DAC output directly to ADC input (wire jumper)
- [ ] Output 1 kHz pilot via DAC, capture via ADC
- [ ] Verify Goertzel f0 magnitude matches expected amplitude
- [ ] Verify 2f0 magnitude is near zero (no distortion)
- [ ] Verify DC magnitude matches DAC bias offset
- [ ] Print harmonics via USART1 for debugging

## Acceptance Criteria
1. Host unit tests pass with <1% magnitude error and <1 degree phase error
2. Pilot tone visible on oscilloscope as clean 1 kHz sine
3. Loopback Goertzel output matches input within 5%
4. Pipeline runs at 32 kSPS without dropping samples
5. CPU utilization measured (should be <30% of DRDY period)
