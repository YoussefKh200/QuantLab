# 🚀 QuantFusion Visualizer — Quick Start Guide

## What You've Got

A complete **web-based visualization dashboard** for your QuantFusion backtesting system. See your results in beautiful interactive charts instead of terminal output.

## 📦 What's Included

```
visualizer/
├── 📊 dashboard.py              ← Main Streamlit app (the UI)
├── 💾 data_export.py            ← Export C++ results to CSV/JSON
├── 🔗 __init__.py               ← Python package setup
├── 🎬 example_export.py         ← Demo showing how to export data
├── ⚙️  setup.py                 ← Automated setup script
├── 🪟 run_dashboard.bat         ← Windows launcher
├── 🐧 run_dashboard.sh          ← Linux/Mac launcher
├── 📋 requirements.txt          ← Python dependencies
├── 📖 README.md                 ← Full documentation
├── 🔧 INTEGRATION_GUIDE.md      ← How to integrate with C++
└── 🚀 QUICKSTART.md             ← This file
```

## ⚡ 3-Minute Setup

### Option 1: Automated Setup (Easiest)

**Windows:**
```powershell
cd visualizer
python setup.py
```

**Linux/Mac:**
```bash
cd visualizer
python setup.py
```

### Option 2: Manual Setup

```bash
# 1. Install dependencies
pip install -r visualizer/requirements.txt

# 2. (Optional) Generate sample data
python visualizer/example_export.py

# 3. Start dashboard
streamlit run visualizer/dashboard.py
```

## 🎯 What You Can Do

### View in 3 clicks:

1. **Run the setup:**
   ```bash
   python visualizer/setup.py
   ```

2. **Start the dashboard:**
   ```bash
   streamlit run visualizer/dashboard.py
   ```

3. **Open browser:**
   - Dashboard opens automatically at `http://localhost:8501`

### Explore Your Data:

- 📈 **Overview**: NAV curve, returns, drawdowns, key metrics
- 💼 **Portfolio**: Exposure tracking, equity/cash, top trades
- ⚠️ **Risk**: VaR, volatility, drawdown analysis
- 🎯 **Signals**: Price action and trading indicators
- 🔬 **Optimization**: Parameter sensitivity, best params
- 📊 **Analysis**: Performance stats, correlation matrix

## 🔌 Integration with Your Code

### Quick Integration (5 lines of code)

After running your backtest, export results:

```python
from visualizer.data_export import QuantFusionDataExporter

exporter = QuantFusionDataExporter("research_db")
exporter.export_nav_curve(timestamps, nav, equity, cash, 
                         gross_exp, net_exp, "my_strategy")
exporter.export_trades(trades, "my_strategy")
```

**That's it!** Open the dashboard and see your results.

## 📊 Data Format

### Export NAV Curve (Required)

```python
exporter.export_nav_curve(
    timestamps=[1672531200, 1672617600, ...],  # Unix timestamps
    nav_values=[100.0, 101.5, 102.3, ...],    # NAV starting at 100
    equity=[1000000, 1015000, ...],           # Equity value in $
    cash=[50000, 51000, ...],                 # Cash in $
    gross_exposure=[0.95, 0.96, ...],         # Total exposure 0-1
    net_exposure=[0.85, 0.86, ...],           # Net exposure -1 to 1
    strategy_name="ma_crossover"
)
```

### Export Trades (Optional)

```python
exporter.export_trades([
    {
        "timestamp": 1672617600,
        "symbol": "SPY",
        "direction": "LONG",
        "entry_price": 450.0,
        "exit_price": 455.0,
        "pnl": 500.0,
        "quantity": 100
    },
    # ... more trades
], "ma_crossover")
```

### Export Risk Metrics (Optional)

```python
exporter.export_risk_metrics({
    "sharpe_ratio": 1.8,
    "max_drawdown": 0.15,
    "var_95": -2450,
    "total_return": 0.45,
    # ... other metrics
})
```

## 🎨 Dashboard Features

### Real Data Import
- Automatically loads from `research_db/` directory
- Supports multiple strategies
- Live refresh with F5

### Interactive Charts
- Zoom, pan, hover for details
- Download as PNG
- Filter by date range

### Metrics & Analytics
- Automated calculation of Sharpe, Sortino, max drawdown
- Monthly returns heatmap
- Correlation matrix
- Win rate analysis

### Multi-View Dashboard
- Switch between tabs for different analyses
- Customizable data sources
- Strategy comparison

## 🔄 Common Workflows

### After Running a Backtest

```python
# 1. Run backtest (your code)
results = my_backtester.run(config)

# 2. Export to visualizer (2-3 lines)
from visualizer.data_export import QuantFusionDataExporter
exporter = QuantFusionDataExporter()
exporter.export_nav_curve(results.timestamps, results.nav, ...)
exporter.export_trades(results.trades, ...)

# 3. Open dashboard
# streamlit run visualizer/dashboard.py
# → Open http://localhost:8501
```

### Comparing Multiple Strategies

```python
# Export each strategy with different name
exporter.export_nav_curve(..., strategy_name="ma_crossover_v1")
exporter.export_nav_curve(..., strategy_name="mean_reversion_v1")
exporter.export_nav_curve(..., strategy_name="breakout_v1")

# Select strategies in sidebar to compare
```

### Optimization Analysis

```python
# Export optimization results
exporter.export_optimization_results(
    param_sets=[[10, 50], [15, 60], ...],
    fitness_scores=[1.8, 1.9, 1.7, ...],
    best_params=[20, 80],
    optimization_name="ma_crossover_optimization"
)

# View parameter heatmaps in Optimization tab
```

## 🐛 Troubleshooting

### "No module named streamlit"
```bash
pip install -r visualizer/requirements.txt
```

### Data not showing in dashboard
1. Check that files exist in `research_db/nav_curves/`
2. Click "Refresh Data" in sidebar
3. Verify CSV format: `python -c "import pandas as pd; print(pd.read_csv('research_db/nav_curves/strategy_nav.csv').head())"`

### Dashboard won't start
```bash
# Try with explicit port
streamlit run visualizer/dashboard.py --server.port 8501
```

### Charts not loading
- Clear browser cache (Ctrl+Shift+Del)
- Refresh page
- Check browser console (F12) for errors

## 📚 Documentation

- **[README.md](README.md)** — Full feature documentation
- **[INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md)** — How to integrate with C++
- **[example_export.py](example_export.py)** — Working code example
- **[data_export.py](data_export.py)** — API reference

## 🎓 Example Walkthrough

### Run the Example

```bash
cd visualizer
python example_export.py
```

This generates sample backtest data in `research_db/`.

### Start Dashboard

```bash
streamlit run dashboard.py
```

### Explore

- Click tabs to switch views
- Use sidebar to filter data
- Click "Refresh Data" to reload

## 🚀 Next Steps

### For Quick Demo
1. Run: `python visualizer/setup.py`
2. Run: `streamlit run visualizer/dashboard.py`
3. Explore sample data

### For Real Integration
1. Read [INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md)
2. Add export calls to your backtester
3. Push data to dashboard

### For Customization
1. Edit `dashboard.py` to add metrics
2. Modify colors, layouts, charts
3. Add custom analysis tabs

## 📞 Quick Reference

| Task | Command |
|------|---------|
| Setup everything | `python visualizer/setup.py` |
| Start dashboard | `streamlit run visualizer/dashboard.py` |
| Generate sample data | `python visualizer/example_export.py` |
| Export backtest results | `from visualizer.data_export import QuantFusionDataExporter` |
| Clear cache | `streamlit cache clear` |
| Stop dashboard | `Ctrl+C` |

## 💡 Tips

- **Speed**: First load takes ~5s (cached after)
- **Scale**: Dashboard handles 100k+ rows efficiently
- **Refresh**: F5 to reload, sidebar "Refresh Data" clears cache
- **Compare**: Export multiple strategies, select them in sidebar
- **Export**: Add exports to your backtest loop for live updates

## ✨ You're Ready!

```bash
# 1. Setup (one time)
python visualizer/setup.py

# 2. After each backtest
exporter.export_nav_curve(...)
exporter.export_trades(...)

# 3. View results
streamlit run visualizer/dashboard.py
# → http://localhost:8501
```

**Questions?** Check README.md or INTEGRATION_GUIDE.md

**Want to customize?** Edit dashboard.py to add your own charts!

---

Made with ❤️ for QuantFusion
