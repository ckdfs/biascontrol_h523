# Scan Artifacts

Bias scan captures and derived figures are grouped here to keep `docs/` tidy.

## Layout

- `raw/`: exported CSV time-series data
- `plots/`: figures generated from the retained captures

## Retention Policy

本目录只保留当前控制算法直接相关、可复现结论的最新验证产物。

当前保留集为：

- `lock_response_<target>_suite5m_*`
  当前固件的 6 组 5 分钟控制测试：
  `quad / min / max / custom45 / custom135 / custom17`
- 少量 plot-only 参考图：
  `vpi_scan_100mvpp_full_3blk_2026-04-02.png`
  `scan_plot_50mvpp.png`
  `scan_plot_100mvpp.png`
  `filtered_h2_scan_50mvpp_3blk_2026-04-03.png`
  `filtered_h2_scan_100mvpp_3blk_2026-04-03.png`

## Naming

- `lock_response_<target>_<tag>_<timestamp>.*`
  `<target>` 为工作点，`<tag>` 为测试批次或修复标签
- `.csv`
  采样后的结构化时间序列
- `.png`
  由 `.csv` 生成的响应曲线图

## Notes

- 运行时采集脚本只保留 `csv`，不再保存 `.txt` 原始串口日志
- 旧的探索性扫描、调参 smoke run、失败试验图和中间推导图不再保留在仓库中
- 若后续产生新的验证批次，应在确认结论稳定后替换当前保留集，而不是持续累积历史产物
