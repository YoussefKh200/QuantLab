# QuantFusion Changelog

## v1.1.0 — Research Operating System Extension

Added 7 new modules implementing the institutional hedge-fund research
infrastructure spec (alpha factory, regime engine, validation, ranking,
lifecycle management, storage, Python bridge).

### New modules

- **`src/regimes/RegimeEngine.h`**
  9-state regime taxonomy (Bull, Bear, Range, VolExpansion,
  VolCompression, RiskOn, RiskOff, LiquidityCrisis, MacroShock).
  Multi-asset feed -> probability vector via vol-ratio, trend strength,
  cross-asset breadth, jump detection, correlation-spike signals,
  combined with sigmoid scoring and exponential smoothing.
  `StrategyRegimeTracker` records per-regime Sharpe for any strategy.

- **`src/alpha_factory/AlphaFactory.h`**
  Automated alpha candidate generation from parameter grids
  (`AlphaSpec::expand()`), parallel evaluation (backtest + factor IC +
  Monte Carlo) via `ThreadPool`, composite scoring, and ranking.
  `default_alpha_specs()` covers momentum, reversion, vol, low-vol,
  breakout, MA-cross, vol-target, nearness, and volume-trend factors.

- **`src/validation/WalkForwardValidator.h`**
  Rolling / Expanding / Nested walk-forward modes. Nested mode runs
  inner k-fold CV for hyperparameter selection before OOS evaluation.
  Produces `WalkForwardResult` with WF Sharpe, WF CAGR, WF max
  drawdown, mean IS/OOS efficiency, overfitting probability, and a
  composite robustness score.

- **`src/validation/RobustnessEngine.h`**
  Monte Carlo return resampling, block bootstrap (preserves
  autocorrelation), trade reshuffling, noise injection, and
  slippage-stress (2x/3x/5x) tests. Each produces a
  `RobustnessResult` with Sharpe percentile bands (p5-p95),
  probability of failure, probability of ruin, and expected/p95
  max drawdown.

- **`src/ranking/StrategyRanker.h`**
  Composite score (Sharpe/Sortino/Calmar/MaxDD/consistency/OOS-Sharpe/
  robustness blend) and institutional fitness score (composite +
  regime stability + diversification benefit + capacity +
  turnover penalty). Correlation-to-portfolio computation for
  diversification scoring.

- **`src/ranking/StrategyRegistry.h`**
  Strategy lifecycle FSM: Research -> Validated -> PaperTrading ->
  Live -> Degraded -> Retired, with full transition history.
  Degradation monitor detects alpha decay (rolling IC vs baseline),
  Sharpe deterioration, execution slippage excess, and regime
  mismatch. Critical alerts auto-transition Live -> Degraded.
  `recommend_deallocation()` / `recommend_retirement()` for portfolio
  management workflows.

- **`src/storage/StorageEngine.h`**
  `FlatFileStorage`: zero-dependency CSV persistence to
  `research_db/{alphas,strategies,trades,nav_curves,walkforward,
  lifecycle}/`. Optional `SQLiteStorage` behind `QL_USE_SQLITE`
  (CMake `-DUSE_SQLITE=ON`).

- **`src/python_bridge/PyBridge.cpp`**
  Pybind11 module `quantfusion` exposing `PyBacktester` (with
  `add_ma_cross` / `add_mean_reversion` / `add_breakout` /
  `add_vol_target` / `add_regime_filtered_ma`), `AlphaFactory`,
  `RegimeEngine`, factor IC research, and portfolio construction
  functions (`equal_weight`, `inverse_vol`, `risk_parity`, `hrp`,
  `min_variance`, `max_sharpe`). Results returned as
  pandas-DataFrame-ready dicts/lists.

### main.cpp — 4 new demos (total 12)

- **Demo 9**: Multi-regime engine — feeds a 9-asset universe through
  `RegimeEngine`, prints regime distribution and latest snapshot.
- **Demo 10**: Alpha Factory — generates ~40 candidates from
  `default_alpha_specs()`, evaluates in parallel, prints top 10 ranked
  alphas with Sharpe/ICIR/composite score.
- **Demo 11**: Full validation pipeline — `WalkForwardValidator`
  (Rolling mode, LHS inner optimizer) -> `RobustnessEngine` (MC,
  bootstrap, trade reshuffle, slippage stress) -> `StrategyRanker`
  (composite + fitness scores) -> `StorageEngine` persistence.
- **Demo 12**: Strategy lifecycle — registers a strategy, walks it
  through Research -> Validated -> PaperTrading -> Live, simulates
  performance degradation after bar 60, shows auto-transition to
  Degraded with CRITICAL/WARNING alerts, and deallocation
  recommendations.

### Bug fixes (cross-module integration)

- **AlphaFactory IC = 0 bug**: `AlphaEngine::compute_ic()` requires
  `scores.size() >= min_universe` (default 20). With small universes
  (e.g. 6-15 assets in demos), IC was always 0. Fixed by setting
  `min_universe = max(2, n_feeds - 1)` for AlphaFactory's internal
  `AlphaEngine` instance.

- **AlphaFactory momentum/ma_cross collision**: both factor types
  mapped to `MACrossStrategy` with identical default params
  (fast=10, slow=50), making "momentum" candidates indistinguishable
  from "ma_cross" candidates regardless of `lookback`. Fixed by
  mapping `momentum`'s `lookback` -> `slow` MA period (with
  `fast = slow/4`), giving momentum candidates parameter sensitivity.

- **Demo 4 "0 bars" bug**: `generate_synthetic()` signature is
  `(ticker, start, n_bars, price, vol, ret, seed, res)`. Demo 4 called
  it as `("AAPL", 175., 0.28, 0.12, 111, start)` — wrong argument
  order/count, with `175.` interpreted as `start` (Timestamp) and
  `0.28` as `n_bars` (truncated to 0). Fixed to
  `("AAPL", start, 1260, 175., 0.28, 0.12, 111)`. Verified: now
  processes 3780 bars (1260 x 3 assets).

- **RobustnessReport::print() zero-sim entries**: `trade_reshuffle`
  returns an empty `RobustnessResult` (n_simulations=0) when fewer
  than 10 trades exist. The report printer now skips these with a
  "(skipped: insufficient data)" note instead of printing all-zero
  metric blocks. `compute_overall_score()` already correctly excluded
  these from the aggregate.

- **Demo 2 silent tearsheet**: `cfg.verbose=false` suppressed
  `BenchmarkRunner`'s tearsheet print (`format_tearsheet` is gated on
  `cfg_.verbose`). Fixed by setting `cfg.verbose=true`.

- **PyBridge.cpp `ICTimeSeries` qualification**: `ICTimeSeries` is
  nested inside `factors::AlphaEngine`, not top-level `factors::`.
  Fixed binding to `factors::AlphaEngine::ICTimeSeries`.

### Project structure additions

```
research/{notebooks,factors,alpha_factory}/   — Python research layer scaffolding
live_trading/                                  — placeholder for live execution bridge
tests/                                         — placeholder for unit tests (BUILD_TESTS)
docs/ARCHITECTURE.md                           — full system docs (diagrams, lifecycle,
                                                  Python integration, deployment workflow)
```

---

## v1.0.0 — Initial Merge (QuantEngine v2 + QuantLab v1)

See prior session notes. Established `ql::` namespace, C++20 core
types, event-driven backtester, 5 strategies, portfolio construction
(EW/InvVol/RiskParity/MinVar/MaxSharpe/HRP), risk engine (VaR/ES/
stress), alpha engine (8 factors + IC), analytics (25+ metrics +
Monte Carlo), microstructure (VPIN/OFI/Kyle/Amihud/Roll/CS), and
optimizer (Grid/Random/LHS/Bayesian + WalkForward template).
