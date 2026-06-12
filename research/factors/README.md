# Custom Factor Definitions

Python-side factor prototypes go here before being ported to C++
`IFactor` implementations in `src/factors/AlphaEngine.h`.

## Workflow

1. Prototype a factor in Python on historical data (pandas/numpy)
2. Validate IC/ICIR using `quantfusion.AlphaEngine` bindings (or
   replicate the Spearman IC calculation in pandas for quick iteration)
3. Once validated, implement as a `LambdaFactor` in C++:

```cpp
auto my_factor = std::make_shared<ql::factors::LambdaFactor>(
    "MyFactor", /*min_bars=*/60,
    [](std::span<const ql::Bar> bars) -> double {
        // factor logic using bars
        return bars.back().volume / bars[bars.size()-20].volume - 1.0;
    });
```

4. Add to `default_alpha_specs()` in `src/alpha_factory/AlphaFactory.h`
   if it should be part of automated alpha generation.
