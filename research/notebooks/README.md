# Research Notebooks

Place Jupyter notebooks here for exploratory factor research, ML model
development, and visualization. Notebooks should import the compiled
`quantfusion` Python module (built via `cmake -DBUILD_PYTHON=ON`).

## Suggested structure

- `01_factor_exploration.ipynb` — IC/ICIR analysis across factor universe
- `02_regime_analysis.ipynb` — RegimeEngine snapshots, per-regime performance
- `03_alpha_factory_review.ipynb` — review AlphaFactory candidates, drill into top-N
- `04_walkforward_diagnostics.ipynb` — WF efficiency, overfitting diagnostics
- `05_portfolio_construction.ipynb` — compare HRP/RiskParity/MaxSharpe allocations

## Example

```python
import sys
sys.path.insert(0, "../build")  # path to compiled quantfusion_py module
import quantfusion as qf
import pandas as pd

feed = qf.generate_synthetic("SPY", qf.from_date(2019,1,2), 1260, 450., 0.17, 0.10, 1001)
cfg = qf.BacktestConfig()
cfg.run_monte_carlo = True
bt = qf.Backtester(cfg)
bt.add_feed(feed)
bt.add_ma_cross(fast=10, slow=50, notional=200_000)
result = bt.run()
nav = pd.DataFrame(result["nav_curve"])
nav.set_index("timestamp")["nav"].plot()
```
