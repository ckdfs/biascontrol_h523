# Scan Artifacts

Bias scan captures and derived figures are grouped here to keep `docs/` tidy.

## Layout

- `raw/`: UART text captures from scan runs
- `plots/`: figures generated from the raw scan files

## Naming

- `scan_100mvpp.*`: 1 kHz pilot at 100 mVpp
- `scan_50mvpp.*`: 1 kHz pilot at 50 mVpp

## Current Plot Set

- `scan_plot_<amp>.png`: 1 kHz dBV, 2 kHz dBV, and DC on one figure
- `scan_diff_plot_<amp>.png`: `1 kHz dBV - 2 kHz dBV` and its inverse
- `scan_h1_over_h2_<amp>.png`: `H1 / H2` voltage ratio
- `scan_h2_over_h1_<amp>.png`: `H2 / H1` voltage ratio
