# QuantFusion Visualizer — File Structure & Summary

## 📁 Directory Layout

```
visualizer/
│
├── 📖 Documentation
│   ├── README.md                 ← Full feature guide
│   ├── QUICKSTART.md            ← 3-minute setup guide (START HERE!)
│   ├── INTEGRATION_GUIDE.md      ← How to connect with C++
│   └── FILE_STRUCTURE.md         ← This file
│
├── 🎨 Dashboard Application
│   └── dashboard.py              ← Main Streamlit web app
│                                   Displays all visualizations
│                                   6 views: Overview, Portfolio, Risk, Signals, Optimization, Analysis
│
├── 💾 Data Management
│   ├── data_export.py            ← Export C++ results → CSV/JSON
│   │                               Classes:
│   │                               - QuantFusionDataExporter (write)
│   │                               - QuantFusionDataLoader (read)
│   │
│   └── __init__.py               ← Python package initialization
│
├── 🚀 Launchers & Setup
│   ├── setup.py                  ← Automated setup script
│   │                               Installs dependencies
│   │                               Creates directories
│   │                               Validates environment
│   │
│   ├── run_dashboard.bat         ← Windows launcher
│   │                               Double-click to start
│   │
│   └── run_dashboard.sh          ← Linux/Mac launcher
│                                   bash run_dashboard.sh
│
├── 📊 Examples & Tests
│   ├── example_export.py         ← Generates sample backtest data
│   │                               Shows complete export workflow
│   │                               Validates data format
│   │
│   └── requirements.txt          ← Python dependencies
│                                   streamlit>=1.28.0
│                                   plotly>=5.17.0
│                                   pandas>=2.0.0
│                                   numpy>=1.24.0
│
└── 📂 Data Directory (auto-created)
    research_db/
    ├── nav_curves/               ← Portfolio NAV curves (CSV)
    ├── trades/                   ← Trade records (CSV)
    ├── alphas/                   ← Alpha signals (CSV)
    ├── strategies/               ← Strategy metadata (JSON)
    └── risk_metrics.json         ← Risk summary (JSON)
```

## 📋 File Descriptions

### Documentation Files

#### `QUICKSTART.md`
**What**: 3-minute setup guide  
**When to read**: First time using visualizer  
**Contains**: Installation, basic usage, quick examples  

#### `README.md`
**What**: Complete feature documentation  
**When to read**: Learning all features  
**Contains**: Features, data formats, troubleshooting  

#### `INTEGRATION_GUIDE.md`
**What**: Integration with C++ backend  
**When to read**: Connecting your backtester  
**Contains**: PyBridge modifications, export examples, architecture  

#### `FILE_STRUCTURE.md`
**What**: This file - directory reference  
**When to read**: Understanding project layout  

### Application Files

#### `dashboard.py` (Main App)
**Purpose**: Streamlit web application  
**Entry point**: `streamlit run dashboard.py`  
**Features**:
- 📈 Overview: NAV, returns, metrics
- 💼 Portfolio: Exposure, equity, trades
- ⚠️ Risk: VaR, volatility, drawdowns
- 🎯 Signals: Price action, indicators
- 🔬 Optimization: Parameter heatmaps
- 📊 Analysis: Performance stats, correlations

**Key Functions**:
- `load_backtest_data()`: Reads from research_db/
- `load_sample_backtest_data()`: Generates demo data
- `st.cache_resource`: Performance optimization

**Dependencies**: streamlit, plotly, pandas, numpy

### Data Management Files

#### `data_export.py` (Core Export Logic)
**Purpose**: Convert C++ results to visualization formats  
**Classes**:
- `QuantFusionDataExporter`: Write data to CSV/JSON
- `QuantFusionDataLoader`: Read from research_db/

**Main Methods**:
```python
exporter.export_nav_curve(...)      # Export portfolio curve
exporter.export_trades(...)         # Export trade records
exporter.export_risk_metrics(...)   # Export risk summary
exporter.export_alpha_signals(...)  # Export indicators
exporter.export_optimization_results(...)  # Export optimization
exporter.export_strategy_metadata(...)     # Export config
```

**Output Formats**: CSV (time series), JSON (metadata/results)

#### `__init__.py`
**Purpose**: Python package initialization  
**Exports**: `QuantFusionDataExporter`, `QuantFusionDataLoader`  
**Usage**: `from visualizer.data_export import QuantFusionDataExporter`

### Launcher Scripts

#### `setup.py`
**Purpose**: Automated environment setup  
**Does**:
- Checks Python version (3.8+)
- Installs dependencies from requirements.txt
- Creates data directories
- Runs example export
- Validates installation

**Usage**: `python visualizer/setup.py`  
**Time**: ~2 minutes

#### `run_dashboard.bat` (Windows)
**Purpose**: Start dashboard on Windows  
**Usage**: Double-click or `run_dashboard.bat`  
**Opens**: http://localhost:8501

#### `run_dashboard.sh` (Linux/Mac)
**Purpose**: Start dashboard on Unix systems  
**Usage**: `bash run_dashboard.sh` or `chmod +x run_dashboard.sh && ./run_dashboard.sh`  
**Opens**: http://localhost:8501

### Example & Requirements

#### `example_export.py`
**Purpose**: Demonstrate complete export workflow  
**Generates**: Sample backtest data in research_db/  
**Shows**:
- Realistic NAV curves
- Trade data
- Risk metrics
- Alpha signals
- Data verification

**Usage**: `python visualizer/example_export.py`  
**Output**: Populates research_db/ with demo data

#### `requirements.txt`
**Purpose**: Python dependencies  
**Packages**:
- `streamlit>=1.28.0`: Web framework
- `plotly>=5.17.0`: Interactive charts
- `pandas>=2.0.0`: Data manipulation
- `numpy>=1.24.0`: Numerical computing

**Install**: `pip install -r visualizer/requirements.txt`

## 🔄 Data Flow

```
C++ Backtester
        ↓
    PyBridge
        ↓
    Python Export
        ↓
    QuantFusionDataExporter
        ↓
    ├── research_db/nav_curves/*.csv
    ├── research_db/trades/*.csv
    ├── research_db/alphas/*.csv
    └── research_db/risk_metrics.json
        ↓
    QuantFusionDataLoader (in dashboard.py)
        ↓
    Streamlit Dashboard
        ↓
    Browser: http://localhost:8501
```

## 📊 Data Format Examples

### NAV Curve CSV (nav_curves/*.csv)
```csv
timestamp,nav,equity,cash,gross_exposure,net_exposure
2023-01-02 00:00:00,100.0,1000000.0,50000.0,0.95,0.85
2023-01-03 00:00:00,101.5,1015000.0,51000.0,0.96,0.86
```

### Trades CSV (trades/*.csv)
```csv
timestamp,symbol,direction,entry_price,exit_price,pnl,quantity
2023-01-15 10:30:00,SPY,LONG,450.0,455.0,500.0,100
```

### Risk Metrics JSON (risk_metrics.json)
```json
{
  "sharpe_ratio": 1.8,
  "max_drawdown": 0.15,
  "total_return": 0.45,
  ...
}
```

## 🚀 Quick Reference

### First Time Setup
1. `python visualizer/setup.py` ← Do this once
2. `streamlit run visualizer/dashboard.py`
3. Open http://localhost:8501

### After Each Backtest
```python
from visualizer.data_export import QuantFusionDataExporter
exporter = QuantFusionDataExporter()
exporter.export_nav_curve(...)
exporter.export_trades(...)
```

### Customize Dashboard
Edit `dashboard.py`:
- Add custom metrics (copy/modify existing code blocks)
- Change colors/themes (modify `template="plotly_dark"`)
- Add new tabs (copy tab structure)

### Troubleshooting
| Issue | Solution |
|-------|----------|
| "No module named streamlit" | `pip install -r visualizer/requirements.txt` |
| Data not appearing | Click "Refresh Data" in sidebar |
| Charts won't load | Clear browser cache (Ctrl+Shift+Del) |
| Dashboard won't start | Check port 8501 is available |

## 📈 Use Cases

### Single Strategy Analysis
1. Run backtest
2. Export with `QuantFusionDataExporter`
3. View in dashboard

### Multi-Strategy Comparison
1. Export each strategy (different names)
2. Select all in sidebar
3. Compare metrics side-by-side

### Parameter Optimization
1. Run optimization
2. Export results with `export_optimization_results`
3. View heatmaps in Optimization tab

### Risk Monitoring
1. Export daily backtest updates
2. Monitor rolling Sharpe, VaR, drawdown
3. Set alerts on dashboard

## 🎯 Next Steps

### Immediate (5 min)
1. Read QUICKSTART.md
2. Run `python visualizer/setup.py`
3. Run `streamlit run visualizer/dashboard.py`

### Short Term (30 min)
1. Read INTEGRATION_GUIDE.md
2. Review example_export.py
3. Add export calls to your backtester

### Medium Term
1. Customize dashboard colors/metrics
2. Add company branding
3. Deploy to cloud (Streamlit Cloud)

## 📞 Getting Help

- **Setup issues**: See QUICKSTART.md
- **Integration questions**: See INTEGRATION_GUIDE.md
- **Feature details**: See README.md
- **Code examples**: See example_export.py
- **API reference**: See data_export.py docstrings

## ✨ Features Summary

| Feature | File | View |
|---------|------|------|
| NAV curve chart | dashboard.py | Overview |
| Drawdown analysis | dashboard.py | Overview |
| Returns distribution | dashboard.py | Overview |
| Exposure tracking | dashboard.py | Portfolio |
| Trade analysis | dashboard.py | Portfolio |
| VaR metrics | dashboard.py | Risk |
| Volatility tracking | dashboard.py | Risk |
| Signal visualization | dashboard.py | Signals |
| Parameter heatmap | dashboard.py | Optimization |
| Performance stats | dashboard.py | Analysis |
| Monthly returns | dashboard.py | Analysis |
| Correlation matrix | dashboard.py | Analysis |

---

**Last Updated**: 2024  
**Version**: 1.0  
**Status**: Ready to use
