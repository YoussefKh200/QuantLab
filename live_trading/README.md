# Live Trading Bridge (Future)

This directory is reserved for the live execution bridge. The design
goal is that **no strategy code changes** when moving from simulation
to live trading — only the data source and execution sink change.

## Planned architecture

```
MarketDataFeed (live)  --> EventBus --> IStrategy (unchanged)
                                            |
                                            v
                                     OrderEvent
                                            |
                                            v
                              ExecutionGateway (FIX/REST)
                                            |
                                            v
                                       FillEvent --> EventBus --> Portfolio
```

- `Clock::set_mode(ClockMode::Live)` switches the simulation clock to
  wall-clock time.
- `MarketSimulator::submit()` is replaced by an `ExecutionGateway`
  implementing the same `submit()/cancel()` interface, routing to a
  broker API (FIX session, REST, or exchange-native protocol).
- `StrategyRegistry::observe()` continues to run every live bar,
  feeding the same degradation monitor used in simulation.
- Reconciliation: live `FillEvent`s flow through the same
  `Portfolio::on_fill()` path as simulated fills, so NAV curves,
  trade records, and analytics are computed identically in both modes.
