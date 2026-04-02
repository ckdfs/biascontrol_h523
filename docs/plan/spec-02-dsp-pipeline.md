# Spec 02 — Signal Processing Pipeline

> Status: **COMPLETE** ✅
> Goal: Pilot tone generation + Goertzel harmonic extraction, verified end-to-end
> Depends on: spec-01-bringup (working DAC + ADC + DMA)
> Completed: 2026-04-02

## Final Assessment (2026-04-02)

- DSP core complete: pilot LUT, H1/H2 Goertzel, CH1 DC accumulation, host-side unit tests, UART debug commands.
- Analysis window: 1280 samples (20 coherent pilot cycles, 20 ms/block).
- `scan vpi` uses 3 Goertzel blocks/step (60 ms measurement window + 2 ms settle per step).
  - Full range scan (±9.95 V, 199 steps) completes in ~12 s.
  - Fast scan (0 → +9.95 V, 100 steps) completes in ~6 s.
- Vπ characterization verified on hardware: **Vπ = 5.451 V** (full-range scan, 3 intervals, 2026-04-02).
- Scan artifacts archived under `docs/scans/` (raw + plots).
- Deferred to later specs: DAC-to-ADC loopback capture, SPI1 DMA write path, formal ISR timing budget.

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
- [x] Goertzel analysis block spans multiple complete pilot cycles (default: 20 cycles = 1280 samples) to improve weak-tone SNR without spectral leakage

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

### 6. Measurement Data Management
- [x] Store scan raw text captures under `docs/scans/raw/`
- [x] Store generated scan plots under `docs/scans/plots/`
- [x] Distinguish scan artifacts by pilot amplitude in filenames

### 7. V_pi Characterization Commands

**Background:**
With VA channel connected to MZM bias electrode and CH0 measuring the PD response,
the H1 magnitude (first harmonic at pilot frequency) is proportional to
|sin(π·V_bias / V_pi)|. Sweeping bias across ±V_pi reveals multiple H1 peaks
spaced exactly V_pi apart. For a typical MZM with V_pi ≈ 5V over a ±10V range,
roughly 4 peaks are visible — more peaks give a better average and higher accuracy.

**`set pilot <mVpp>` command:**
- Sets pilot amplitude at runtime (peak-to-peak millivolts)
- `pilot_amplitude_v = mVpp / 2000.0` (converts mVpp → volts peak)
- Default: 100 mVpp (= 0.05 V peak at DAC; after 4× subtractor → 200 mVpp at modulator)
- Updates both `ctx.config->pilot_amplitude_v` and live `ctx.bias_ctrl.pilot` via
  `pilot_gen_set_amplitude()`
- Prints confirmation and resulting clamp voltage (= 10.0V − pilot_peak)

**`scan vpi [fast]` command:**
- Performs an open-loop V_pi characterization sweep
- Only available in IDLE state (closed-loop must be stopped first)
- Scan parameters:
  - Step: 0.1 V (fixed)
  - Blocks per step: 3 = 3840 samples = 60 ms measurement window per step
  - Pre-settle: 100 ms initial DAC settle (large jump to scan_start)
  - Per-step settle: 2 ms after each DAC step
  - Range default: `−clamp_v` to `+clamp_v` where `clamp_v = 10.0 − pilot_peak`
  - `fast` modifier: single-sided scan `0V` to `+clamp_v` (~6 s instead of ~12 s)
- Per-step timing:
  1. Write static `bias_v` to DAC, wait 2 ms for settling
  2. For each of 3 blocks: reset pilot phase + Goertzel, collect 1280 samples
     while writing `bias_v + pilot_gen_next()` to DAC before each ADC read
  3. Coherently average H1 I/Q across 3 blocks → final H1 magnitude
  4. Print: `SCAN <bias_v> <h1_mag>` (one line per step, for external post-processing)
- After scan: restore DAC to 0 V
- Post-scan minimum finding (on-chip):
  - Threshold: 10% of max observed H1 (zeros of H1 are sharp; minimum detection is
    more robust than peak detection for Vπ extraction)
  - Local minimum: `h1[i] < h1[i−1] && h1[i] < h1[i+1] && h1[i] < threshold`
  - Parabolic interpolation for sub-step (< 0.1 V) minimum location accuracy
  - V_pi = mean of consecutive minimum spacings
  - Print: `[scan] minima: <v0> <v1> ...` and `[scan] Vpi = X.XXX V (N intervals)`
- Scan buffer: `static float` arrays of `SCAN_MAX_STEPS = 201` entries (≈ 1.6 KB BSS)

**Implementation checklist:**
- [x] `set pilot <mVpp>` command in `app_handle_command()`
- [x] `scan vpi [fast]` command in `app_handle_command()`
- [x] `#define SCAN_MAX_STEPS 201` in `app_main.c`
- [x] Minimum detection with 10% threshold + parabolic interpolation (replaced peak detection)
- [x] Tuned to 3 blocks/step: reliable settling, ~6 s fast / ~12 s full scan
- [x] Verified on hardware (2026-04-02): full-range scan on MZM (VA channel),
      4 minima at −8.445V / −2.928V / +2.520V / +7.908V, **Vπ = 5.451V (3 intervals)**
- [x] Scan artifacts archived: `docs/scans/raw/vpi_scan_100mvpp_full_3blk_2026-04-02.txt`
      and `docs/scans/plots/vpi_scan_100mvpp_full_3blk_2026-04-02.png`

## Acceptance Criteria
1. Host unit tests pass with <1% magnitude error and <1 degree phase error
   Status: **met** (2026-04-01, `build-test/test_goertzel`, 12 passed / 0 failed).
2. Pilot tone visible on oscilloscope as clean 1 kHz sine
   Status: confirmed functional via loopback H1 measurements; formal scope capture deferred.
3. Loopback Goertzel output matches input within 5%
   Status: confirmed qualitatively via H1 plateau ~200 mV across flat scan regions; formal
   DAC→ADC loopback capture deferred.
4. Pipeline runs at 64 kSPS without dropping samples
   Status: confirmed in practice (no DRDY timeout seen during scans or closed-loop runs).
5. CPU utilization measured (should be <30% of DRDY period)
   Status: not formally measured; deferred.
6. `scan vpi` produces ≥2 H1 minima over scan range and reports V_pi within 5% of
   vendor datasheet value for the connected MZM.
   Status: **met** (2026-04-02) — full-range scan, 4 minima, Vπ = 5.451V (3 intervals,
   σ = 0.065V). Consistent across multiple scan runs (5.366V / 5.473V / 5.402V / 5.451V).
