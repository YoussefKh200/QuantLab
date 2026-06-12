# QuantFusion Dashboard — Web Visualizer

> Interactive web-based visualization system for QuantFusion backtesting, risk analysis, and optimization.

[![Streamlit App](https://img.shields.io/badge/Streamlit-App-FF4B4B.svg?style=for-the-badge)](http://localhost:8501)
[![Python 3.8+](https://img.shields.io/badge/Python-3.8%2B-blue.svg?style=for-the-badge)](https://www.python.org)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

---

## 🎯 Overview

QuantFusion Dashboard replaces terminal output with a professional, interactive web interface. Visualize your backtesting results in real-time with 6 different analysis views.

```
C++ Backtester → PyBridge → Data Export → Web Dashboard → Browser
```

### Why Use This?

| Before | After |
|--------|-------|
| Terminal output | Beautiful interactive charts |
| Print statements | Professional metrics & KPIs |
| CSV files | Live data visualization |
| Manual analysis | Automated calculations |

---

## ✨ Features

### 6 Dashboard Views

#### 📈 **Overview** — Performance at a Glance
- Portfolio NAV equity curve
- Daily returns distribution
- Drawdown analysis over time
- Key metrics: Return, Sharpe, Max DD, Win Rate, Sortino

#### 💼 **Portfolio** — Holdings & Exposure
- Gross vs. net exposure tracking
- Equity vs. cash allocation
- Top winning & losing trades
- Trade P&L distribution

#### ⚠️ **Risk** — Risk Metrics & Analysis
- Value at Risk (VaR) at 95% confidence
- Expected Shortfall (CVaR)
- Rolling 20-day volatility
- Drawdown over time

#### 🎯 **Signals** — Strategy Signals & Indicators
- Price action with moving averages
- Trading signals (Long/Short/Neutral)
- Alpha signal distribution
- Technical indicators overlay

#### 🔬 **Optimization** — Parameter Analysis
- Parameter sensitivity heatmaps
- Top parameter sets ranking
- Fitness score distribution
- Best parameter recommendations

#### 📊 **Analysis** — Detailed Statistics
- Performance statistics table
- Monthly returns heatmap
- Asset correlation matrix
- Sharpe & Sortino ratio trends

### Interactive Features
✅ Zoom, pan, and hover on all charts  
✅ Download charts as PNG  
✅ Filter by time period  
✅ Compare multiple strategies  
✅ Dark professional theme  
✅ Responsive design (desktop, tablet, mobile)  

---

## 🚀 Quick Start

### 1. Installation (1 minute)

```bash
# Navigate to visualizer directory
cd visualizer

# Automated setup (installs dependencies, creates directories)
python setup.py
```

### 2. Run Dashboard (30 seconds)

```bash
# Start the Streamlit app
streamlit run dashboard.py

# Dashboard opens automatically at http://localhost:8501
```

### 3. Explore (2 minutes)

- Click tabs to switch views
- Use sidebar to filter data
- Click "Refresh Data" to reload
- Try "Refresh" button to clear cache

---

## 💾 Data Export

### Export from C++ Backtester

After running a backtest, export results with 3 lines of Python:

```python
from visualizer.data_export import QuantFusionDataExporter

exporter = QuantFusionDataExporter("research_db")

# Export portfolio NAV curve
exporter.export_nav_curve(
    timestamps=backtest_results.timestamps,
    nav_values=backtest_results.nav,
    equity=backtest_results.equity,
    cash=backtest_results.cash,
    gross_exposure=backtest_results.gross_exposure,
    net_exposure=backtest_results.net_exposure,
    strategy_name="ma_crossover_v1"
)

# Export trades
exporter.export_trades(
    trades=backtest_results.trades,
    strategy_name="ma_crossover_v1"
)
```

### Data Format

The dashboard expects data in `research_db/` directory:

```
research_db/
├── nav_curves/
│   └── strategy_name_nav.csv          # Portfolio NAV curve
├── trades/
│   └── strategy_name_trades.csv       # Trade records
├── alphas/
│   └── strategy_name_signals.csv      # Alpha signals & indicators
├── strategies/
│   └── strategy_name_metadata.json    # Strategy configuration
└── risk_metrics.json                  # Risk summary metrics
```

#### NAV Curve CSV Format

```csv
timestamp,nav,equity,cash,gross_exposure,net_exposure
2023-01-02 00:00:00,100.0,1000000.0,50000.0,0.95,0.85
2023-01-03 00:00:00,101.5,1015000.0,51000.0,0.96,0.86
```

**Fields:**
- `timestamp` — ISO 8601 datetime
- `nav` — Net Asset Value (starting at 100)
- `equity` — Portfolio equity in dollars
- `cash` — Cash reserves in dollars
- `gross_exposure` — Total absolute exposure (0–1)
- `net_exposure` — Net market exposure (-1–1)

#### Trades CSV Format

```csv
timestamp,symbol,direction,entry_price,exit_price,pnl,quantity
2023-01-15 10:30:00,SPY,LONG,450.0,455.0,500.0,100
2023-01-20 14:20:00,QQQ,SHORT,380.0,375.0,300.0,60
```

**Fields:**
- `timestamp` — ISO 8601 datetime
- `symbol` — Ticker symbol (SPY, QQQ, etc.)
- `direction` — LONG or SHORT
- `entry_price` — Entry price per unit
- `exit_price` — Exit price per unit
- `pnl` — Profit/loss in dollars
- `quantity` — Number of shares

---

## 📖 Usage Guide

### Starting the Dashboard

**Option 1: Automated (Recommended)**
```bash
python visualizer/setup.py        # One-time setup
streamlit run dashboard.py        # Start dashboard
```

**Option 2: Manual**
```bash
pip install -r requirements.txt
streamlit run dashboard.py
```

**Option 3: Windows Batch File**
```bash
run_dashboard.bat
```

**Option 4: Linux/Mac Shell Script**
```bash
bash run_dashboard.sh
```

### Dashboard Navigation

1. **Select View** (Sidebar)
   - Choose: Overview, Portfolio, Risk, Signals, Optimization, Analysis

2. **Data Source** (Sidebar)
   - Live: Reads from research_db/
   - Sample: Uses demo data

3. **Time Period** (Sidebar)
   - Filter: 1M, 3M, 6M, YTD, 1Y, All

4. **Strategy Filter** (Sidebar)
   - Select multiple strategies to compare

5. **Refresh** (Sidebar)
   - Click to reload data from disk

### Interpreting Metrics

| Metric | Interpretation | Good Value |
|--------|-----------------|------------|
| **Sharpe Ratio** | Return per unit of risk | > 1.0 |
| **Sortino Ratio** | Return per unit of downside risk | > 1.0 |
| **Max Drawdown** | Largest peak-to-trough loss | < 20% |
| **Win Rate** | Percentage of profitable days | > 50% |
| **Total Return** | Overall profit/loss | > 0% |
| **Volatility** | Annual standard deviation of returns | 10–15% |

---

## 🔧 Customization

### Add Custom Metrics

Edit `dashboard.py` to add new KPIs:

```python
# In Overview section, add:
col6, col7, col8 = st.columns(3)

with col6:
    st.metric("Profit Factor", f"{profit_factor:.2f}")

with col7:
    st.metric("Recovery Factor", f"{recovery_factor:.2f}")

with col8:
    st.metric("Calmar Ratio", f"{calmar_ratio:.2f}")
```

### Change Color Scheme

Find and replace `template="plotly_dark"` with other Plotly templates:
- `plotly_dark` (current)
- `plotly` (light)
- `plotly_white` (clean)
- `ggplot2` (minimal)

### Add New Dashboard Tab

```python
# In dashboard mode selection:
elif dashboard_mode == "🆕 Custom":
    st.header("My Custom Analysis")
    
    # Add your charts here
    fig = px.scatter(data, x="param1", y="param2")
    st.plotly_chart(fig, use_container_width=True)
```

### Export Custom Charts

All Plotly charts have a camera icon to download as PNG.

---

## 📊 API Reference

### QuantFusionDataExporter

```python
from visualizer.data_export import QuantFusionDataExporter

exporter = QuantFusionDataExporter("research_db")
```

**Methods:**

```python
# Export NAV curve (required)
exporter.export_nav_curve(
    timestamps: List[float],
    nav_values: List[float],
    equity: List[float],
    cash: List[float],
    gross_exposure: List[float],
    net_exposure: List[float],
    strategy_name: str = "strategy_1"
) -> Path

# Export trades
exporter.export_trades(
    trades: List[Dict],
    strategy_name: str = "strategy_1"
) -> Path

# Export risk metrics
exporter.export_risk_metrics(
    metrics: Dict[str, Any]
) -> Path

# Export alpha signals
exporter.export_alpha_signals(
    timestamps: List[float],
    signals: Dict[str, List[float]],
    strategy_name: str = "strategy_1"
) -> Path

# Export optimization results
exporter.export_optimization_results(
    param_sets: List[List[float]],
    fitness_scores: List[float],
    best_params: List[float],
    optimization_name: str = "optimization_1"
) -> Path
```

### QuantFusionDataLoader

```python
from visualizer.data_export import QuantFusionDataLoader

loader = QuantFusionDataLoader("research_db")
```

**Methods:**

```python
# Load all NAV curves
nav_curves = loader.load_nav_curves()  # List[DataFrame]

# Load all trades
trades = loader.load_trades()  # List[DataFrame]

# Load risk metrics
metrics = loader.load_risk_metrics()  # Dict

# Load alpha signals
signals = loader.load_alpha_signals()  # List[DataFrame]

# Get strategy summary
summary = loader.get_strategy_summary(nav_df)  # Dict
```

---

## ⚙️ Configuration

### Requirements

```
streamlit>=1.28.0
plotly>=5.17.0
pandas>=2.0.0
numpy>=1.24.0
```

### System Requirements

- **Python**: 3.8 or higher
- **OS**: Windows, Linux, macOS
- **Browser**: Chrome, Firefox, Safari, Edge
- **RAM**: 2GB minimum (4GB+ recommended)
- **Disk**: 500MB for dependencies

### Environment Setup

```bash
# Create virtual environment (optional)
python -m venv venv

# Activate (Windows)
venv\Scripts\activate

# Activate (Linux/Mac)
source venv/bin/activate

# Install dependencies
pip install -r visualizer/requirements.txt
```

---

## 🐛 Troubleshooting

### "No module named streamlit"

**Solution:**
```bash
pip install -r visualizer/requirements.txt
```

### Data not appearing in dashboard

**Steps:**
1. Verify files exist: `ls research_db/nav_curves/`
2. Check format: `python -c "import pandas as pd; print(pd.read_csv('research_db/nav_curves/strategy_nav.csv').head())"`
3. Click "Refresh Data" button
4. Clear cache: `streamlit cache clear`

### Dashboard won't start

**Solution:**
```bash
# Specify a different port if 8501 is busy
streamlit run dashboard.py --server.port 8502
```

### Charts look wrong

**Steps:**
1. Clear browser cache (Ctrl+Shift+Delete)
2. Refresh page (F5)
3. Check browser console (F12) for errors
4. Try a different browser

### Performance is slow

**Tips:**
- Reduce time period (use sidebar filter)
- Decrease number of strategies (deselect in sidebar)
- Archive old backtest results
- Close other browser tabs

---

## 📁 File Structure

```
visualizer/
├── dashboard.py              # Main Streamlit application (600+ lines)
├── data_export.py            # Data export utilities (350+ lines)
├── example_export.py         # Demo showing complete workflow
├── setup.py                  # Automated setup script
├── requirements.txt          # Python dependencies
├── run_dashboard.bat         # Windows launcher
├── run_dashboard.sh          # Unix launcher
├── __init__.py               # Python package initialization
├── README.md                 # This file
├── QUICKSTART.md             # 3-minute setup guide
├── INTEGRATION_GUIDE.md      # C++ integration instructions
├── FILE_STRUCTURE.md         # Detailed file reference
└── START_HERE.md             # Quick overview
```

---

## 🔄 Integration Example

### Complete Workflow

```python
#!/usr/bin/env python3
"""
Example: Export backtest results and visualize
"""

from visualizer.data_export import QuantFusionDataExporter
import my_backtester

# Step 1: Run backtest
print("Running backtest...")
results = my_backtester.run(config)

# Step 2: Export results
print("Exporting to visualizer...")
exporter = QuantFusionDataExporter("research_db")

exporter.export_nav_curve(
    timestamps=results.timestamps,
    nav_values=results.nav,
    equity=results.equity,
    cash=results.cash,
    gross_exposure=results.gross_exposure,
    net_exposure=results.net_exposure,
    strategy_name=f"{results.strategy_name}_v1"
)

exporter.export_trades(
    trades=results.trades,
    strategy_name=results.strategy_name
)

exporter.export_risk_metrics({
    "sharpe_ratio": results.sharpe_ratio,
    "max_drawdown": results.max_drawdown,
    "total_return": results.total_return,
    # ... other metrics
})

# Step 3: Open dashboard
print("Open dashboard at http://localhost:8501")
print("Run: streamlit run visualizer/dashboard.py")
```

---

## 📈 Performance Tips

1. **Export in batches**: Don't export every bar, batch exports every 10–50 bars
2. **Archive results**: Move old backtests to separate directories
3. **Limit history**: Keep last 2–3 years in active analysis
4. **Compress data**: CSV files compress to ~10% of original size
5. **Use filters**: Filter by time period to reduce data loaded

---

## 🎯 Best Practices

✅ **DO:**
- Export after each complete backtest
- Name strategies clearly (e.g., `ma_crossover_v2_tuned`)
- Use consistent timestamp format (Unix seconds)
- Monitor live dashboard during optimization
- Archive old results regularly

❌ **DON'T:**
- Export incomplete or invalid data
- Change file locations after export
- Edit CSV files manually
- Export extremely large datasets (>1M rows) without filtering
- Run multiple dashboard instances on same port

---

## 🔗 Related Documentation

- **[QUICKSTART.md](QUICKSTART.md)** — 3-minute setup guide
- **[INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md)** — C++ integration details
- **[FILE_STRUCTURE.md](FILE_STRUCTURE.md)** — Complete file reference
- **[START_HERE.md](START_HERE.md)** — Overview & quick links

---

## 📊 Example Use Cases

### Single Strategy Analysis
```bash
python setup.py
exporter.export_nav_curve(...)  # Export results
streamlit run dashboard.py      # View in dashboard
```

### Multi-Strategy Comparison
```python
# Export multiple strategies
exporter.export_nav_curve(..., strategy_name="ma_crossover")
exporter.export_nav_curve(..., strategy_name="mean_reversion")
exporter.export_nav_curve(..., strategy_name="breakout")

# Select all in dashboard sidebar to compare
```

### Parameter Optimization
```python
# Export optimization results
exporter.export_optimization_results(
    param_sets=param_sweep,
    fitness_scores=results,
    best_params=best,
    optimization_name="ma_crossover_optim"
)
# View heatmaps in Optimization tab
```

### Risk Monitoring
```python
# Export daily backtest updates
for date in trading_dates:
    results = run_daily_backtest(date)
    exporter.export_nav_curve(...)  # Daily update
# Monitor rolling risk metrics
```

---

## 💬 Support & FAQ

**Q: Can I use this with live trading?**  
A: Yes! Export your live trading results using the same API.

**Q: How do I deploy to the cloud?**  
A: Use Streamlit Cloud, Heroku, or AWS Elastic Beanstalk.

**Q: Can I customize the colors?**  
A: Yes! Edit `dashboard.py` and modify Plotly templates.

**Q: Is this real-time?**  
A: It updates when you refresh the page or click "Refresh Data".

**Q: How much data can it handle?**  
A: Tested with 100k+ rows. Performance depends on chart complexity.

**Q: Can I export to PDF?**  
A: Use browser print feature or export individual charts as PNG.

---

## 📜 License

MIT License - See LICENSE file for details

---

## 🙏 Acknowledgments

Built for [QuantFusion](https://github.com/yourusername/quantfusion) backtesting system.

Powered by:
- [Streamlit](https://streamlit.io/) — App framework
- [Plotly](https://plotly.com/) — Interactive charts
- [Pandas](https://pandas.pydata.org/) — Data manipulation
- [NumPy](https://numpy.org/) — Numerical computing

---

## 🚀 Getting Started

1. **Quick Demo** (5 min):
   ```bash
   python visualizer/setup.py
   streamlit run visualizer/dashboard.py
   ```

2. **Integrate Your Code** (30 min):
   - Read [INTEGRATION_GUIDE.md](INTEGRATION_GUIDE.md)
   - Add 3 lines to your backtester
   - Export and visualize

3. **Customize** (1 hour):
   - Edit `dashboard.py`
   - Add your metrics
   - Deploy

---

**Status**: ✅ Production Ready  
**Version**: 1.0.0  
**Last Updated**: 2024  

**Next Step**: Read [QUICKSTART.md](QUICKSTART.md) to get started!
