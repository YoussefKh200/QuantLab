# QuantFusion — Quantitative Research Operating System
## Architecture Reference (v1)

---

## 1. System Overview

QuantFusion is a research operating system for systematic strategy
development. It combines a high-performance C++20 simulation core with
a Python research layer (via pybind11), organised around a single
pipeline: **generate → evaluate → validate → rank → construct → monitor**.

```
┌────────────────────────────────────────────────────────────────────────┐
│                         PYTHON RESEARCH LAYER                          │
│   notebooks/  factor research, ML, visualization, orchestration        │
└───────────────────────────────┬────────────────────────────────────────┘
                                  │ pybind11 (PyBridge.cpp)
┌─────────────────────────────────▼────────────────────────────────────────┐
│                        C++20 SIMULATION CORE                            │
│                                                                          │
│  ┌──────────────┐  ┌───────────────┐  ┌────────────────────────────┐  │
│  │ DataManager  │─▶│  AlphaFactory  │─▶│  WalkForwardValidator       │  │
│  │ DataPipeline │  │  (generate +   │  │  RobustnessEngine            │  │
│  │ (LRU, parallel│  │   evaluate +   │  │  (rolling/expanding/nested,  │  │
│  │  enrichment) │  │   rank alphas) │  │   MC/bootstrap/stress)        │  │
│  └──────┬───────┘  └────────┬───────┘  └──────────────┬─────────────┘  │
│         │                    │                          │                │
│         ▼                    ▼                          ▼                │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                       Backtester                                  │  │
│  │  EventBus → MarketSimulator → Portfolio → RiskEngine              │  │
│  │  IStrategy implementations (MA/MeanRev/Breakout/VolTarget/        │  │
│  │  CrossSectional/RegimeFilteredMA/PortfolioAware)                  │  │
│  └──────────────────────────────┬───────────────────────────────────┘  │
│                                   │                                       │
│         ┌─────────────────────────┼─────────────────────────┐           │
│         ▼                          ▼                          ▼           │
│  ┌─────────────┐         ┌──────────────────┐       ┌──────────────────┐ │
│  │ RegimeEngine│         │  StrategyRanker   │       │ StrategyRegistry  │ │
│  │ (9 states,  │────────▶│  (composite +     │──────▶│ (lifecycle +      │ │
│  │  per-regime │         │   fitness scores) │       │  degradation      │ │
│  │  Sharpe)    │         └──────────────────┘       │  monitor)         │ │
│  └─────────────┘                                     └──────────────────┘ │
│                                                                          │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    StorageEngine (FlatFile / SQLite)              │  │
│  │  research_db/{alphas, strategies, trades, nav_curves,             │  │
│  │               walkforward, lifecycle}                             │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Module Hierarchy

```
src/
├── core/
│   ├── Types.h              — Bar, Tick, Order, Fill, Position, concepts
│   ├── SymbolRegistry.h      — InstrumentID <-> ticker (FNV-1a hash)
│   ├── Clock.h               — nanosecond simulation clock + latency model
│   ├── LockFree.h            — SPSC/MPMC queues
│   └── ThreadPool.h          — work-stealing pool, parallel_for, map
│
├── events/
│   └── EventBus.h            — priority event queue (Market<Risk<Signal<
│                                Order<Fill<Portfolio<System)
│
├── data/
│   ├── DataManager.h         — CSV/synthetic loader, LRU cache
│   └── DataPipeline.h        — parallel enriched-bar preprocessing
│                                (VWAP, ATR14, RSI14, Bollinger, RelVol)
│
├── execution/
│   ├── SlippageEngine.h      — Fixed/Spread/VolParticip/SqrtImpact/2-Comp
│   └── OrderBook.h           — IntraBarSynthesizer, SimulatedOrderBook,
│                                TickExecutionEngine
│
├── simulation/
│   ├── MarketSimulator.h     — order matching (Market/Limit/Stop/IOC/FOK,
│   │                            partial fills, latency)
│   ├── Backtester.h          — orchestrator: feeds + strategies + events
│   ├── ExampleStrategies.h   — MACross, MeanRev, Breakout, VolTarget,
│   │                            CrossSectional
│   ├── RegimeDetection.h     — soft 2-state regime + RegimeFilteredMA
│   └── PortfolioStrategy.h   — PortfolioAwareStrategy (dynamic alloc)
│
├── portfolio/
│   ├── Portfolio.h           — NAV, positions, PnL, equity curve
│   └── PortfolioConstruction.h — EW/InvVol/RiskParity/MinVar/MaxSharpe/
│                                  HRP/BlackLitterman
│
├── risk/
│   └── RiskEngine.h          — pre-trade checks, VaR/ES, stress, Kelly,
│                                vol targeting
│
├── factors/
│   └── AlphaEngine.h         — IFactor, 8 canonical factors, IC/ICIR,
│                                cross-sectional normalisation
│
├── analytics/
│   ├── Analytics.h           — 25+ metrics, Monte Carlo
│   └── BenchmarkComparison.h — BuyAndHold/60-40 benchmarks, tearsheet
│
├── microstructure/
│   └── Microstructure.h      — VPIN, OFI, Kyle's lambda, Amihud, Roll, CS
│
├── optimizer/
│   └── Optimizer.h           — Grid/Random/LHS/Bayesian, WalkForward<T>
│
├── regimes/                              ★ NEW
│   └── RegimeEngine.h        — 9-state regime taxonomy (Bull/Bear/Range/
│                                VolExpansion/VolCompression/RiskOn/RiskOff/
│                                LiquidityCrisis/MacroShock), per-strategy
│                                regime performance tracker
│
├── alpha_factory/                        ★ NEW
│   └── AlphaFactory.h        — automated alpha generation (param grids),
│                                parallel evaluation, IC research,
│                                Monte Carlo, composite ranking
│
├── validation/                           ★ NEW
│   ├── WalkForwardValidator.h — Rolling/Expanding/Nested WF, robustness
│   │                             score, overfitting probability
│   └── RobustnessEngine.h    — MC resampling, block bootstrap, trade
│                                reshuffle, noise injection, slippage stress
│
├── ranking/                              ★ NEW
│   ├── StrategyRanker.h      — composite score + institutional fitness
│   │                            score, correlation-to-portfolio
│   └── StrategyRegistry.h    — lifecycle FSM (Research->Validated->
│                                PaperTrading->Live->Degraded->Retired),
│                                degradation alerts, deallocation/retirement
│
├── storage/                              ★ NEW
│   └── StorageEngine.h       — FlatFileStorage (CSV), optional SQLite
│                                (QL_USE_SQLITE)
│
└── python_bridge/                        ★ NEW
    └── PyBridge.cpp           — pybind11 module exposing Backtester,
                                  AlphaFactory, RegimeEngine, factor IC,
                                  portfolio construction
```

---

## 3. Data Flow: Research Pipeline

```
                    ┌─────────────────┐
                    │   DataPipeline   │   parallel load + enrich
                    └────────┬─────────┘
                              │ vector<DataFeed>
                              ▼
                    ┌─────────────────┐
                    │   AlphaFactory   │   generate(specs) -> AlphaIDs
                    │                  │   evaluate(ids, feeds) [parallel]
                    │                  │   rank() -> composite_score
                    └────────┬─────────┘
                              │ top(N) AlphaCandidates
                              ▼
              ┌───────────────────────────────┐
              │  Promote to IStrategy          │
              │  (AlphaCandidate -> Strategy)  │
              └───────────────┬─────────────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │ WalkForwardValidator │  Rolling/Expanding/Nested
                    │                      │  -> WalkForwardResult
                    └──────────┬───────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  RobustnessEngine    │  MC, bootstrap, stress
                    │                      │  -> RobustnessReport
                    └──────────┬───────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │   StrategyRanker     │  composite + fitness scores
                    │                      │  correlation-to-portfolio
                    └──────────┬───────────┘
                               │ qualified(min_fitness)
                               ▼
                    ┌─────────────────────┐
                    │ PortfolioConstruction│  HRP / RiskParity / MaxSharpe
                    │ (allocate capital)   │
                    └──────────┬───────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  StrategyRegistry    │  Research->Validated->
                    │  (lifecycle FSM)     │  PaperTrading->Live
                    └──────────┬───────────┘
                               │ observe() every live bar
                               ▼
                    ┌─────────────────────┐
                    │ DegradationMonitor   │  alpha decay / Sharpe
                    │ (built into Registry)│  deterioration / regime
                    │                      │  mismatch -> alerts ->
                    │                      │  auto-Degraded -> dealloc
                    └──────────┬───────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │   StorageEngine      │  persist all of the above
                    │   (research_db/)     │  to CSV / SQLite
                    └─────────────────────┘
```

---

## 4. Strategy Lifecycle State Machine

```
         register_strategy()
                │
                ▼
         ┌────────────┐
         │  RESEARCH  │◀────────────────┐
         └─────┬──────┘                  │
               │ transition(Validated)   │ transition(Research)
               ▼                          │ "send back for rework"
         ┌────────────┐                  │
         │ VALIDATED  │                  │
         └─────┬──────┘                  │
               │ transition(PaperTrading)│
               ▼                          │
         ┌────────────────┐              │
         │ PAPER_TRADING   │──────────────┘
         └─────┬───────────┘
               │ transition(Live)
               ▼                                    set_baseline()
         ┌────────────┐    auto on Critical alert   captures Sharpe,
         │    LIVE    │───────────────────────────▶ │  IC, slippage,
         └─────┬──────┘    (sharpe_deterioration)   │  best regime
               │                                      ▼
               │                              ┌────────────┐
               │  transition(Live)            │  DEGRADED  │
               │◀──────────────────────────── └─────┬──────┘
               │  "manual recovery"                  │
               │                                      │ recommend_retirement()
               ▼                                      ▼ if Sharpe<0 for
         ┌────────────┐                        ┌────────────┐
         │  RETIRED   │◀───────────────────────│  RETIRED   │
         └────────────┘   (terminal, any state) └────────────┘
```

Every transition is recorded in `RegisteredStrategy::history` with a
timestamp and reason string, persisted via
`StorageEngine::persist_lifecycle()`.

---

## 5. Class Hierarchy: Strategies

```
                    IStrategy (abstract)
                    ├── on_start(sim, portfolio)
                    ├── on_bar(bar, sim, portfolio)   [pure virtual]
                    ├── on_fill(fill)
                    ├── on_risk(risk_event)
                    └── on_end(sim, portfolio)
                         │
       ┌──────────┬──────┴───────┬────────────────┬─────────────────┐
       ▼          ▼               ▼                ▼                  ▼
MACrossStrategy  MeanReversion  BreakoutStrategy  VolTargetTrend   CrossSectional
                  Strategy                          Strategy        Strategy
       │
       └──▶ RegimeFilteredMAStrategy (extends MA logic with RegimeDetector)

PortfolioAwareStrategy — independent multi-asset strategy using
                         construction::{equal_weight,inverse_vol,
                         risk_parity,hrp,max_sharpe}
```

`AlphaFactory::make_strategy()` is a factory function that converts an
`AlphaCandidate` (factor_type + params) into a concrete `IStrategy`
instance — this is the bridge between automated alpha generation and
the backtesting core.

---

## 6. Python / C++ Integration Design

### Boundary Philosophy
- **C++ owns**: simulation, execution modeling, risk calculation,
  optimization, Monte Carlo — anything that benefits from compiled
  performance and tight loops.
- **Python owns**: orchestration, exploratory analysis, ML model
  training, visualization, notebook-driven research.
- **Data crosses the boundary as plain dicts/lists** (convertible
  directly to `pandas.DataFrame`), not live C++ object references —
  this avoids lifetime/GIL complications and keeps the binding surface
  small and stable.

### Example: Research workflow

```python
import quantfusion as qf
import pandas as pd

# 1. Generate / load data (C++ synthetic generator or CSV via DataManager)
feed = qf.generate_synthetic("SPY", qf.from_date(2019,1,2), 1260,
                              450.0, 0.17, 0.10, seed=1001)

# 2. Configure and run a backtest
cfg = qf.BacktestConfig()
cfg.initial_cash = 1_000_000
cfg.run_monte_carlo = True

bt = qf.Backtester(cfg)
bt.add_feed(feed)
bt.add_ma_cross(fast=10, slow=50, notional=200_000, size_by_vol=True)
result = bt.run()

# 3. Convert to pandas for analysis
nav_df    = pd.DataFrame(result["nav_curve"])
trades_df = pd.DataFrame(result["trades"])
report    = result["report"]
print(f"Sharpe: {report['sharpe_ratio']:.3f}  CAGR: {report['cagr']:.2f}%")

# 4. Regime-aware analysis
regime_engine = qf.RegimeEngine()
# feed bars in, get RegimeState snapshots ...

# 5. Alpha factory — automated research
factory = qf.AlphaFactory()
factory.run([feed])
factory.print_top(10)
top_ids = factory.top(5)
```

### Zero-copy considerations
For tick-level or factor-matrix data where Python needs large arrays,
expose `py::array_t<double>` views over contiguous C++ buffers
(`DataFeed::bars()` is already `std::vector<Bar>` — contiguous and
amenable to `py::array_t` wrapping without copies). The current
`PyBridge.cpp` uses dict/list conversion for simplicity in v1; a
`export_factor_matrix()` zero-copy path is the natural v2 extension.

---

## 7. Deployment Workflow

```
1. RESEARCH
   - AlphaFactory.run(feeds, specs) generates + ranks candidates
   - Researchers inspect top candidates via print_top() / Python notebooks

2. VALIDATION
   - Promote candidate -> IStrategy via AlphaFactory::make_strategy()
   - WalkForwardValidator.run(feeds) -> WalkForwardResult
   - RobustnessEngine.run(...) -> RobustnessReport
   - registry.transition(id, Validated, "WF Sharpe 0.8, robustness 0.65")

3. RANKING & CAPITAL ALLOCATION
   - StrategyRanker.add(entry) for each validated strategy
   - ranker.rank(portfolio_returns) -> composite + fitness scores
   - ranker.qualified(min_fitness=0.4) -> capital candidates
   - construction::hrp(cov) or risk_parity(cov) -> target weights

4. PAPER TRADING
   - registry.transition(id, PaperTrading, "queued for paper")
   - Run live data feed through Backtester in simulation mode with
     live market data (MarketSimulator unchanged — same code path)

5. LIVE
   - registry.transition(id, Live, "approved by PM")
   - registry.set_baseline(id, {sharpe, mean_ic, avg_slippage_bps,
                                  best_regime})
   - registry.set_allocation(id, pct, total_capital)

6. MONITORING (every live bar)
   - regime_engine.snapshot(ts) -> RegimeState
   - registry.observe(id, bar_return, factor_ic, realized_slippage_bps,
                       &regime_state) -> alerts
   - On Critical alert: auto-transition to Degraded
   - registry.recommend_deallocation() / recommend_retirement()

7. PERSISTENCE
   - storage.persist_alphas(...) / persist_rankings(...) /
     persist_walkforward(...) / persist_lifecycle(registry)
   - All CSVs land in research_db/{alphas,strategies,trades,
     nav_curves,walkforward,lifecycle}/
```

---

## 8. Regime Taxonomy Detail

| Regime | Trigger Signals | Typical Strategy Response |
|---|---|---|
| Bull | trend>0.3, vol_ratio<1.2, breadth>0.2 | Trend-following long bias |
| Bear | trend<-0.3, vol_ratio>1.2, breadth<-0.2 | Reduce gross, hedge, short bias |
| Range | \|trend\|<0.15, vol_ratio<1.0 | Mean-reversion strategies favoured |
| VolExpansion | vol_ratio>1.5 | Reduce position sizing, widen stops |
| VolCompression | vol_ratio<0.7, vol_short<0.20 | Pre-breakout positioning |
| RiskOn | breadth>0.30, vol_short<0.20 | Increase risk-asset allocation |
| RiskOff | breadth<-0.20, vol_ratio>1.3 | Flight to quality |
| LiquidityCrisis | vol_short>0.40 AND corr_spike>0.80 | De-risk aggressively, widen slippage assumptions |
| MacroShock | jump_z>3.0 AND vol_ratio>2.0 | Halt new entries, reassess |

`RegimeEngine::snapshot()` returns a full probability vector
(`RegimeProbVec`) — strategies can act on `dominant()` or weight
decisions by the full distribution for smoother transitions.

---

## 9. Build & Run

```bash
# Core build (no Python/SQLite)
g++ -std=c++20 -O3 -march=native -I. main.cpp -o quantfusion

# CMake with optional features
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
         -DUSE_SQLITE=ON \
         -DBUILD_PYTHON=ON
make -j$(nproc)
```

```bash
./quantfusion          # all 12 demos
./quantfusion 9        # Multi-regime engine (9-state classification)
./quantfusion 10       # Alpha Factory — automated generation & ranking
./quantfusion 11       # Walk-forward + robustness + strategy ranking
./quantfusion 12       # Strategy lifecycle & degradation monitor
```

---

## 10. Scaling to Thousands of Strategies

- `AlphaFactory::evaluate()` parallelises across `ThreadPool` — each
  candidate's backtest runs independently (embarrassingly parallel).
- `StrategyRanker` operates on lightweight `StrategyEntry` summaries
  (no full NAV curves retained after scoring) — O(N log N) ranking
  for N in the thousands.
- `StorageEngine::FlatFileStorage` writes one CSV row per
  strategy/alpha — trivially shardable; SQLite backend provides
  indexed queries when the flat-file approach becomes unwieldy.
- `StrategyRegistry::observe()` is O(1) per strategy per bar —
  thousands of live strategies can be monitored in a single pass
  using rolling deques bounded by `rolling_window`.
- Future distributed extension: `AlphaFactory::generate()` produces
  candidate IDs that can be sharded across worker processes/machines,
  each running `evaluate()` on a partition and writing results to a
  shared `StorageEngine` (SQLite or future PostgreSQL backend) for
  central `rank()`.
