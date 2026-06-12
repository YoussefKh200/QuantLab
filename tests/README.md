# Unit Tests

Enable with `cmake -DBUILD_TESTS=ON` (requires GTest).

## Suggested test coverage

- `test_types.cpp`            — Bar/Tick/Order validity, Timestamp arithmetic
- `test_symbol_registry.cpp`  — FNV-1a hash collisions, ticker round-trip
- `test_event_bus.cpp`        — priority ordering, subscribe/dispatch
- `test_market_simulator.cpp` — order type fill logic, partial fills
- `test_portfolio.cpp`        — NAV/PnL/cost-basis correctness
- `test_analytics.cpp`        — Sharpe/Sortino/Calmar against known fixtures
- `test_portfolio_construction.cpp` — weight sums to 1, risk parity
                                       converges to equal risk contribution
- `test_alpha_engine.cpp`     — Spearman IC against scipy reference values
- `test_regime_engine.cpp`    — probability vector sums to 1, dominant()
                                 correctness on synthetic regime sequences
- `test_walkforward.cpp`      — window slicing correctness, no overlap
                                 between IS and OOS
- `test_strategy_registry.cpp`— lifecycle FSM valid/invalid transitions,
                                 degradation alert triggers
