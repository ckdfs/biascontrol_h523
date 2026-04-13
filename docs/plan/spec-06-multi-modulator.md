# Spec 06 — Multi-Modulator Support

> Status: **Future**
> Goal: Extend to DDMZM, DPMZM, DPQPSK, PM modulator types
> Depends on: spec-05-robustness (stable MZM platform)

## New Files

| File | Description |
|------|-------------|
| `control/src/ctrl_modulator_ddmzm.c` | Dual-drive MZM strategy |
| `control/src/ctrl_modulator_dpmzm.c` | Dual-parallel MZM strategy |
| `control/src/ctrl_modulator_dpqpsk.c` | DP-QPSK strategy |
| `control/src/ctrl_modulator_pm.c` | Phase modulator strategy |

## Modulator Types Overview

### DDMZM (Dual-Drive MZM)
- 2 bias channels (upper/lower arm)
- Pilot on one arm, lock both to quadrature
- Error: cross-correlation of H1 between arms

### DPMZM (Dual-Parallel MZM)
- 3 bias channels (MZM-I quad, MZM-Q quad, parent phase)
- Multi-frequency pilot: f1 for MZM-I, f2 for MZM-Q, beat for parent
- Requires 2 Goertzel frequency sets
- Sequential locking: children first, then parent

### DPQPSK (Dual-Polarization QPSK)
- 6+ bias channels (2x DPMZM)
- Extension of DPMZM strategy with polarization awareness
- Highest complexity, longest development

### PM (Phase Modulator)
- No MZM transfer curve
- Pilot detection via modulation index estimation
- Different error function entirely

## Task Checklist

### Infrastructure
- [ ] Multi-frequency Goertzel: support 2+ pilot frequencies simultaneously
- [ ] Multi-channel DAC coordination: lock N channels in sequence or parallel
- [x] Modulator type selection: UART command `set mod <type>` — implemented
- [x] Strategy registry: `modulator_get_strategy(type)` lookup — implemented

### Per-Modulator Implementation
- [ ] DDMZM: implement strategy, test on hardware
- [ ] DPMZM: implement strategy, test on hardware
- [ ] DPQPSK: implement strategy, test on hardware
- [ ] PM: implement strategy, test on hardware

## Adding a New Modulator

Follow the Strategy Pattern defined in `control/inc/ctrl_modulator.h`:

1. Create `control/src/ctrl_modulator_<name>.c` and matching `.h`
2. Implement `modulator_strategy_t`:
   - `compute_error()`: harmonic data → error signal
   - `is_locked()`: harmonic data → bool
   - `init()`: set up context
3. Add enum entry to `modulator_type_t`
4. Register in `modulator_get_strategy()` lookup table
5. No changes needed in `ctrl_bias.c` or `app_main.c`

## Acceptance Criteria
1. Each modulator type locks on real hardware
2. Modulator switching works via UART without reboot
3. All modulator types achieve < 2 degree accuracy
