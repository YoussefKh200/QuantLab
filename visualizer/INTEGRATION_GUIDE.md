# QuantFusion Visualizer — Integration Guide

## Overview

The QuantFusion Dashboard visualizer connects to your C++ backtester via the PyBridge Python interface. This guide shows how to integrate your backtesting results into the web-based visualization system.

## Architecture

```
C++ Backtester           PyBridge              Python Exporter       Streamlit Dashboard
┌──────────────┐         ┌──────────┐         ┌─────────────────┐   ┌─────────────────┐
│ Backtester   │────────▶│ PyBridge │────────▶│ DataExporter    │───▶│ Dashboard       │
│ Results      │         │ (C++→Py) │         │ (format data)   │   │ (web UI)        │
└──────────────┘         └──────────┘         └─────────────────┘   └─────────────────┘
                                                      │
                                                      ▼
                                                ┌─────────────────┐
                                                │ research_db/    │
                                                │ ├── nav_curves/ │
                                                │ ├── trades/     │
                                                │ ├── alphas/     │
                                                │ └── strategies/ │
                                                └─────────────────┘
```

## Step 1: Modify PyBridge.cpp

Add export functionality to your PyBridge to expose backtest results to Python:

```cpp
// In src/python_bridge/PyBridge.cpp

#include "../path/to/data_export.h"  // Your export utilities
#include <pybind11/pybind11.h>

PYBIND11_MODULE(quantfusion_py, m) {
    m.def("export_nav_curve", [](
        const std::vector<double>& timestamps,
        const std::vector<double>& nav,
        const std::vector<double>& equity,
        const std::vector<double>& cash,
        const std::vector<double>& gross_exp,
        const std::vector<double>& net_exp,
        const std::string& strategy_name
    ) {
        // Call Python exporter
        py::object data_exporter = py::module::import("visualizer.data_export");
        py::object exporter = data_exporter.attr("QuantFusionDataExporter")("research_db");
        exporter.attr("export_nav_curve")(
            timestamps, nav, equity, cash, gross_exp, net_exp, strategy_name
        );
    });
    
    m.def("export_trades", [](
        const std::vector<py::dict>& trades,
        const std::string& strategy_name
    ) {
        auto data_exporter = py::module::import("visualizer.data_export");
        auto exporter = data_exporter.attr("QuantFusionDataExporter")("research_db");
        exporter.attr("export_trades")(trades, strategy_name);
    });
}
```

## Step 2: Export Results from Your Backtester

After running a backtest, export the results:

```python
# Python script using PyBridge

from visualizer.data_export import QuantFusionDataExporter
import quantfusion_py as qf

# Run your backtest (via PyBridge)
backtest_results = qf.run_backtest(config)

# Export results
exporter = QuantFusionDataExporter("research_db")

# Export NAV curve
exporter.export_nav_curve(
    timestamps=backtest_results.timestamps,
    nav_values=backtest_results.nav,
    equity=backtest_results.equity,
    cash=backtest_results.cash,
    gross_exposure=backtest_results.gross_exposure,
    net_exposure=backtest_results.net_exposure,
    strategy_name=f"{backtest_results.strategy_name}_v1"
)

# Export trades
exporter.export_trades(
    trades=[
        {
            "timestamp": t.timestamp,
            "symbol": t.symbol,
            "direction": "LONG" if t.quantity > 0 else "SHORT",
            "entry_price": t.entry_price,
            "exit_price": t.exit_price,
            "pnl": t.pnl,
            "quantity": abs(t.quantity)
        }
        for t in backtest_results.trades
    ],
    strategy_name=backtest_results.strategy_name
)

# Export risk metrics
exporter.export_risk_metrics({
    "sharpe_ratio": backtest_results.metrics.sharpe_ratio,
    "sortino_ratio": backtest_results.metrics.sortino_ratio,
    "max_drawdown": backtest_results.metrics.max_drawdown,
    "var_95": backtest_results.metrics.var_95,
    "cvar_95": backtest_results.metrics.cvar_95,
    "total_return": backtest_results.metrics.total_return,
    "annual_volatility": backtest_results.metrics.annual_volatility,
    "win_rate": backtest_results.metrics.win_rate
})

# Export alpha signals
exporter.export_alpha_signals(
    timestamps=backtest_results.timestamps,
    signals={
        "ma_fast": backtest_results.indicators.ma_fast,
        "ma_slow": backtest_results.indicators.ma_slow,
        "signal": backtest_results.signals.trading_signal,
        "rsi": backtest_results.indicators.rsi
    },
    strategy_name=backtest_results.strategy_name
)

# Export optimization results (if applicable)
exporter.export_optimization_results(
    param_sets=optimization_results.param_sets,
    fitness_scores=optimization_results.fitness_scores,
    best_params=optimization_results.best_params,
    optimization_name="ma_crossover_optimization"
)
```

## Step 3: Run the Dashboard

Once data is exported:

```bash
# Install dependencies
pip install -r visualizer/requirements.txt

# Start the dashboard
cd visualizer
streamlit run dashboard.py

# Dashboard opens at http://localhost:8501
```

## Step 4: Customize for Your Needs

### Adding Custom Metrics

Edit `dashboard.py` to add your own metrics:

```python
# In dashboard.py, add to Overview section:

col6, col7 = st.columns(2)
with col6:
    st.metric("Profit Factor", f"{profit_factor:.2f}")
with col7:
    st.metric("Recovery Factor", f"{recovery_factor:.2f}")
```

### Custom Chart Types

Add new chart sections:

```python
st.subheader("Custom Analysis")
fig = px.scatter(
    x=nav_df["timestamp"],
    y=nav_df["net_exposure"],
    color=nav_df["nav"].pct_change(),
    title="Exposure vs Daily Returns"
)
st.plotly_chart(fig, use_container_width=True)
```

## Data Format Reference

### NAV Curve CSV

```csv
timestamp,nav,equity,cash,gross_exposure,net_exposure
2023-01-01 00:00:00,100.0,100000.0,5000.0,0.95,0.85
2023-01-02 00:00:00,101.5,102000.0,5100.0,0.96,0.86
...
```

**Fields:**
- `timestamp`: ISO 8601 datetime
- `nav`: Net Asset Value (starting at 100)
- `equity`: Portfolio equity value in dollars
- `cash`: Cash reserves in dollars
- `gross_exposure`: Total absolute exposure (0-1)
- `net_exposure`: Net market exposure (-1 to 1)

### Trades CSV

```csv
timestamp,symbol,direction,entry_price,exit_price,pnl,quantity
2023-01-15 10:30:00,SPY,LONG,450.0,455.0,500.0,100
2023-01-20 14:20:00,QQQ,SHORT,380.0,375.0,300.0,60
...
```

**Fields:**
- `timestamp`: ISO 8601 datetime
- `symbol`: Asset ticker (e.g., SPY, QQQ)
- `direction`: LONG or SHORT
- `entry_price`: Entry price per unit
- `exit_price`: Exit price per unit
- `pnl`: Profit/loss in dollars
- `quantity`: Number of shares

### Risk Metrics JSON

```json
{
  "sharpe_ratio": 1.8,
  "sortino_ratio": 2.1,
  "max_drawdown": 0.15,
  "var_95": -0.025,
  "cvar_95": -0.035,
  "total_return": 0.45,
  "annual_volatility": 0.12,
  "win_rate": 0.55,
  "num_days": 252,
  "start_date": "2023-01-01",
  "end_date": "2023-12-31"
}
```

## Example Integration Flow

### 1. Minimal Integration

```python
from visualizer.data_export import QuantFusionDataExporter
import my_backtester

# Run backtest
results = my_backtester.run()

# Export for visualization
exporter = QuantFusionDataExporter()
exporter.export_nav_curve(
    results.timestamps,
    results.nav,
    results.equity,
    results.cash,
    results.gross_exp,
    results.net_exp,
    "my_strategy"
)
exporter.export_trades(results.trades, "my_strategy")
```

### 2. Full Integration

```python
from visualizer.data_export import QuantFusionDataExporter
import my_backtester

class BacktestRunner:
    def __init__(self):
        self.exporter = QuantFusionDataExporter()
    
    def run_and_visualize(self, strategy_config):
        # Run backtest
        results = my_backtester.run(strategy_config)
        
        # Export all components
        self._export_results(results)
        
        # Print visualization URL
        print("View results: http://localhost:8501")
        return results
    
    def _export_results(self, results):
        """Export all result types"""
        self.exporter.export_nav_curve(
            results.timestamps, results.nav, results.equity,
            results.cash, results.gross_exp, results.net_exp,
            results.strategy_name
        )
        self.exporter.export_trades(results.trades, results.strategy_name)
        self.exporter.export_risk_metrics(results.risk_metrics)
        self.exporter.export_alpha_signals(
            results.timestamps,
            results.indicators,
            results.strategy_name
        )
```

## Real-time Updates

To enable live dashboard updates during backtesting:

```python
from visualizer.data_export import QuantFusionDataExporter
import threading

class LiveBacktestVisualizer:
    def __init__(self):
        self.exporter = QuantFusionDataExporter()
        self.update_interval = 10  # Update every 10 bars
    
    def on_backtest_update(self, backtest_state):
        """Called periodically during backtest"""
        if backtest_state.bar % self.update_interval == 0:
            self.exporter.export_nav_curve(
                backtest_state.timestamps,
                backtest_state.nav_values,
                # ... other data
            )
```

## Troubleshooting

### Data not appearing in dashboard

1. Check file paths:
   ```python
   from visualizer.data_export import QuantFusionDataExporter
   exporter = QuantFusionDataExporter()
   print(exporter.output_dir)  # Should print 'research_db'
   ```

2. Verify CSV format:
   ```python
   import pandas as pd
   df = pd.read_csv("research_db/nav_curves/my_strategy_nav.csv")
   print(df.head())
   ```

3. Check Streamlit cache:
   - Click "Refresh Data" button in sidebar
   - Or clear cache: `streamlit cache clear`

### Dashboard crashes with large datasets

- Filter by time period in sidebar
- Reduce number of plots on single page
- Use sample data for testing

## Performance Tips

1. **Export in batches**: Don't export single bars, batch exports every N bars
2. **Use compression**: CSV files are gzipped by default
3. **Limit history**: Keep last 2-3 years of data in active analysis
4. **Archive old results**: Move old backtests to separate directory

## Next Steps

- ✅ Install visualizer: `pip install -r visualizer/requirements.txt`
- ✅ Run example: `python visualizer/example_export.py`
- ✅ Start dashboard: `streamlit run visualizer/dashboard.py`
- ✅ Integrate with your backtester
- ✅ Add custom charts and metrics

## Support

For issues or questions:
1. Check README.md in visualizer/ directory
2. Review example_export.py for reference
3. Consult data_export.py documentation
