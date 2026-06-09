# QuantLab — Institutional Quantitative Research Platform
## Architecture Reference

---

## System Overview

```
┌──────────────────────────────────────────────────────────────────────┐
│                    QuantLab Platform                                  │
│                                                                        │
│  ┌─────────────┐   ┌─────────────┐   ┌──────────────┐               │
│  │  DataManager│   │  EventBus   │   │  ThreadPool  │               │
│  │  (LRU Cache)│──▶│ (PriorityQ) │◀──│ (Work-Queue) │               │
│  └─────────────┘   └──────┬──────┘   └──────────────┘               │
│         │                  │                                           │
│         ▼                  ▼                                           │
│  ┌─────────────┐   ┌──────────────┐   ┌──────────────┐               │
│  │  DataFeed   │   │MarketSimulator│  │ AlphaEngine  │               │
│  │ (per symbol)│   │(Order Matching│  │ (IC, Decay,  │               │
│  └─────────────┘   │ + LOB Sim)   │  │  X-sectional)│               │
│                    └──────┬───────┘  └──────────────┘               │
│                           │                                            │
│                    ┌──────▼───────┐                                   │
│                    │  Portfolio   │   ┌──────────────┐               │
│                    │  (NAV, PnL,  │──▶│  RiskEngine  │               │
│                    │   Positions) │   │  (VaR, ES,   │               │
│                    └──────┬───────┘   │   Limits)    │               │
│                           │           └──────────────┘               │
│                    ┌──────▼───────┐   ┌──────────────┐               │
│                    │  Analytics   │   │  Optimizer   │               │
│                    │  (Tearsheet, │   │  (Grid, LHS, │               │
│                    │   Monte Carlo│   │   Bayesian,  │               │
│                    │   Attribution│   │   Walk-Fwd)  │               │
│                    └──────────────┘   └──────────────┘               │
└──────────────────────────────────────────────────────────────────────┘
```

---

## Module Hierarchy

```
src/
├── core/
│   ├── Types.h           — Canonical types (Bar, Tick, Order, Fill, Position)
│   │                       C++20 concepts (BarLike, TimeSeries)
│   ├── SymbolRegistry.h  — Thread-safe InstrumentID ↔ ticker mapping
│   │                       FNV-1a hashing, corporate action tracking
│   ├── Clock.h           — Deterministic simulation clock (ns precision)
│   │                       Configurable latency model (network/exchange/OMS)
│   ├── LockFree.h        — SPSC ring buffer, MPMC bounded queue
│   └── ThreadPool.h      — Work-stealing pool, parallel_for, map, latch sync
│
├── events/
│   └── EventBus.h        — Priority event queue (market→risk→signal→order→fill)
│                           Typed pub/sub, deterministic replay
│
├── data/
│   └── DataManager.h     — CSV/binary loader, LRU cache, parallel loading
│                           Survivorship-bias safe universe snapshots
│                           GBM + regime synthetic data generator
│
├── execution/
│   └── SlippageEngine.h  — Fixed / Spread / Volume / Sqrt-Impact / Two-Component
│                           Pluggable ISlippageModel interface
│                           CommissionConfig (per-share, SEC fee, ECN rebate)
│
├── simulation/
│   ├── MarketSimulator.h — Per-instrument order books, all order types
│   │                       Partial fills, IOC/FOK, latency modeling
│   ├── Backtester.h      — Main orchestrator: feeds + strategies + events
│   └── ExampleStrategies.h — MACross, CrossSectional, VolTarget, MeanRev
│
├── portfolio/
│   ├── Portfolio.h           — NAV, cash, positions, realized PnL, equity curve
│   │                           FIFO cost basis, margin/buying power
│   └── PortfolioConstruction.h — Equal, InvVol, RiskParity, MinVar, MaxSharpe
│                                  HRP (Hierarchical Risk Parity), Black-Litterman
│
├── risk/
│   └── RiskEngine.h      — Pre-trade checks, daily loss halt, max DD halt
│                           Historical VaR, Parametric VaR, ES, Monte Carlo VaR
│                           Stress testing (GFC, COVID, Dotcom scenarios)
│                           Kelly sizing, volatility targeting
│
├── factors/
│   └── AlphaEngine.h     — IFactor interface, LambdaFactor adapter
│                           Momentum, Reversal, Vol, LowVol, VWAP, Seasonality
│                           Cross-sectional: winsorize, z-score, rank normalize
│                           IC / ICIR time series analysis
│
├── analytics/
│   └── Analytics.h       — Full tearsheet: 25+ metrics
│                           Monte Carlo: return resampling, path simulation
│
├── optimizer/
│   └── Optimizer.h       — GridSearch, RandomSearch, LHS, BayesianOptimizer
│                           WalkForward<OptimizerT> template
│
└── microstructure/
    └── Microstructure.h  — VPIN, OFI, Kyle's lambda, Amihud, Roll, CS spread
                            IntraBarSynthesizer, SimulatedOrderBook
```

---

## Design Patterns

| Pattern | Where Used | Rationale |
|---------|-----------|-----------|
| **Event-driven** | EventBus, Backtester | Causal ordering, no lookahead bias |
| **Strategy pattern** | ISlippageModel, IFactor, IStrategy | Pluggable behavior without engine changes |
| **Observer / pub-sub** | EventBus::subscribe | Decouples fill routing, risk alerts |
| **Singleton** | Clock, SymbolRegistry, ThreadPool | One source of time truth; shared state |
| **Factory** | DataManager::generate_synthetic | Hides construction complexity |
| **Template method** | WalkForward<OptimizerT> | Optimizer-agnostic walk-forward |
| **LRU Cache** | DataManager | Memory-bounded bar storage |
| **CRTP / Concepts** | BarLike, TimeSeries | Zero-overhead abstraction in C++20 |
| **Lock-free SPSC** | LockFree.h | Market-data → strategy pipeline |
| **Object pool** | (future) Order recycling | Avoid per-order allocation in hot path |

---

## Event Priority Ordering

```
Timestamp T:
  Priority 0: MarketDataEvent   ← raw bar arrives
  Priority 1: RiskEvent         ← pre-trade check
  Priority 2: SignalEvent       ← alpha signal fires
  Priority 3: OrderEvent        ← order submitted
  Priority 4: FillEvent         ← fill confirmed
  Priority 5: PortfolioEvent    ← NAV updated
  Priority 9: SessionEvent      ← BOD/EOD housekeeping

Events at the same nanosecond are processed in priority order.
This is the key guarantee against look-ahead bias and race conditions.
```

---

## Latency Model

```
Order submission:
  strategy.submit() → +50µs (OMS processing) → +100µs (network) → exchange

Exchange matching:
  order arrives → +5µs (matching engine) → fill generated

Fill delivery:
  fill → +100µs (network) → +50µs (OMS) → strategy.on_fill()

Total round-trip: ~305µs modeled latency (configurable per scenario)
```

---

## Execution Assumptions (Bar-level simulation)

```
Market order:  fills at next bar open ± slippage
Limit order:   fills if bar low/high crosses limit_price
Stop order:    triggers if bar range crosses stop_price, then market fill
IOC:           fill immediately or cancel residual
GTC:           persists across bars until filled or cancelled

Partial fills: capped at 10% of bar volume per bar (configurable)
Slippage:      applied directionally (buy pays more, sell receives less)
               clamped to [bar.low, bar.high]
```

---

## Scalability Decisions

### Horizontal (more assets)
- `DataManager` loads feeds in parallel via `ThreadPool`
- `AlphaEngine` computes factors per-instrument in parallel
- `MarketSimulator` keeps per-instrument pending order maps (O(1) lookup)
- No global lock in the hot path — only DataManager and SymbolRegistry use mutexes

### Vertical (more data per asset)
- `DataFeed` stores bars in contiguous `std::vector<Bar>` for cache locality
- Binary search on sorted bars: O(log N) range queries
- LRU eviction in DataManager caps memory at configured limit
- SPSC queue for MD→strategy path eliminates contention

### Tick data extension
Replace `DataFeed::bars()` → `DataFeed::ticks()` and wire `MarketSimulator::on_tick()`.
No strategy interface changes required — `on_bar()` is synthesized from ticks via
a bar aggregator that sits between the feed and the strategy layer.

### Live trading extension
1. Replace `Clock::ClockMode::Simulation` → `ClockMode::Live`
2. Replace `DataFeed` with a network feed (FIX/FAST/SBE parser)
3. Replace `MarketSimulator::submit()` → FIX session send
4. Wire `FillEvent` from exchange acknowledgments
5. All strategy and portfolio code is unchanged

---

## Key Metrics Computed

**Returns**: Total Return, CAGR, Annualized Vol  
**Risk-Adjusted**: Sharpe, Sortino, Calmar, MAR, Omega, Ulcer Index  
**Drawdown**: Max DD, Avg DD, Max DD Duration, Recovery Factor  
**Trades**: Win Rate, Profit Factor, Expectancy, Avg Win/Loss, Best/Worst  
**Relative**: Alpha, Beta, Correlation, Tracking Error, IR, Treynor, Up/Down Capture  
**VaR/ES**: Historical, Parametric, Monte Carlo — 1-day 99%, 10-day 95%  
**Factor**: IC, IC Std, ICIR, decay curve  

---

## Build

```bash
# C++20, single translation unit
g++ -std=c++20 -O3 -march=native -I. main.cpp -o quantlab

# CMake (when CMakeLists.txt added)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Run Demos

```bash
./quantlab          # All demos
./quantlab 1        # MA Crossover + vol-targeting + Monte Carlo
./quantlab 2        # Cross-sectional 12-1 momentum L/S on 10 assets  
./quantlab 3        # Walk-forward optimization (LHS, 252-bar IS, 63-bar OOS)
./quantlab 4        # Alpha factor IC analysis — 6 factors, 20 assets
./quantlab 5        # Portfolio construction comparison (6 methods)
./quantlab 6        # Microstructure metrics + slippage model comparison
```

## Adding a Custom Strategy

```cpp
class MyStrategy : public ql::IStrategy {
public:
    void on_bar(const ql::Bar& bar, ql::MarketSimulator& sim,
                ql::Portfolio& pf) override
    {
        // Your signal logic here
        if (signal_fires) {
            double qty = std::floor(100'000.0 / bar.close);
            buy_market(bar.instrument, qty);  // helper from IStrategy
        }
    }
    void on_end(ql::MarketSimulator& sim, ql::Portfolio& pf) override {
        // Close all positions
    }
};

// Register and run
ql::Backtester bt(cfg);
bt.add_feed(data_mgr().generate_synthetic("SPY", start, 252));
bt.add_strategy(std::make_shared<MyStrategy>(), "my_strat");
auto result = bt.run();
```

## Adding a Custom Alpha Factor

```cpp
auto my_factor = std::make_shared<ql::factors::LambdaFactor>(
    "MyFactor",       // name
    60,               // min bars required
    [](std::span<const ql::Bar> bars) -> double {
        // Return a scalar score — higher = stronger signal
        return bars.back().volume / bars[bars.size()-20].volume - 1.0;
    });

alpha_engine.add_factor(my_factor);
auto ic = alpha_engine.compute_ic("MyFactor", feeds, start, end);
std::cout << "ICIR = " << ic.icir << "\n";
```
