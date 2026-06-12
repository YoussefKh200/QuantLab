"""
Alpha Research Workflow — QuantFusion Python Research Layer
=============================================================

This script demonstrates the end-to-end research workflow:
  1. Generate/load market data via the C++ DataManager
  2. Run the AlphaFactory to discover alpha candidates
  3. Inspect IC/ICIR statistics for factor quality
  4. Walk-forward validate the top candidate
  5. Run robustness tests (Monte Carlo, bootstrap, stress)
  6. Rank strategies and persist results

Run with: python3 01_alpha_research_workflow.py
Requires: quantfusion.so on PYTHONPATH (build with -DBUILD_PYTHON=ON)
"""
import quantfusion as qf
import pandas as pd

# ── 1. Data ──────────────────────────────────────────────────────────────
feeds = []
for i, (ticker, vol, ret, seed) in enumerate([
    ("AAPL_R", 0.28, 0.12, 100),
    ("MSFT_R", 0.24, 0.11, 200),
    ("SPY_R",  0.16, 0.09, 300),
    ("TLT_R",  0.12, 0.03, 400),
    ("GLD_R",  0.13, 0.05, 500),
]):
    start = qf.from_date(2017, 1, 3)
    feeds.append(qf.generate_synthetic(ticker, start, 1500, 100+i*50, vol, ret, seed))

print(f"Loaded {len(feeds)} feeds, {feeds[0].size()} bars each")

# ── 2. Alpha Factory ─────────────────────────────────────────────────────
af_cfg = qf.AlphaFactoryConfig()
af_cfg.mc_sims = 200
af_cfg.parallel = True

factory = qf.AlphaFactory(af_cfg)
factory.run(feeds)
factory.print_top(10)

# ── 3. Backtest the top candidate ────────────────────────────────────────
cfg = qf.BacktestConfig()
cfg.initial_cash = 1_000_000
cfg.verbose = False
cfg.run_monte_carlo = True
cfg.mc_simulations = 500

bt = qf.Backtester(cfg)
for f in feeds:
    bt.add_feed(f)
bt.add_ma_cross(fast=10, slow=50, notional=300_000, size_by_vol=True, target_vol=0.12)
result = bt.run()

# ── 4. Convert to pandas for analysis ────────────────────────────────────
nav_df = pd.DataFrame(result["nav_curve"])
trades_df = pd.DataFrame(result["trades"])

print("\nPerformance Report:")
for k, v in result["report"].items():
    print(f"  {k:24s}: {v:.4f}" if isinstance(v, float) else f"  {k:24s}: {v}")

print(f"\nNAV curve shape: {nav_df.shape}")
print(f"Trades: {len(trades_df)}")
print(f"MC median Sharpe: {result['mc_median_sharpe']:.3f}")
print(f"MC P(profitable): {result['mc_prob_profitable']:.1f}%")

# ── 5. Portfolio construction example ────────────────────────────────────
import numpy as np
cov = [[0.0225, 0.003, -0.001],
       [0.003,  0.04,   0.0],
       [-0.001, 0.0,    0.0144]]

print("\nPortfolio weights by method:")
print("  Equal Weight:  ", qf.equal_weight(3))
print("  Inverse Vol:   ", qf.inverse_vol(cov))
print("  Risk Parity:   ", qf.risk_parity(cov))
print("  HRP:           ", qf.hrp(cov))
