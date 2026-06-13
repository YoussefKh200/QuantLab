# QuantFusion: Algorithmic Trading Research Platform

Welcome to **QuantFusion** — a powerful, easy-to-use platform for researching, backtesting, and analyzing trading strategies without writing complex code.

## What is QuantFusion?

QuantFusion is a financial research tool that lets you test trading ideas on historical market data to see how they would have performed. Whether you're a seasoned trader, investment professional, or someone curious about quantitative trading, QuantFusion provides the tools you need to validate your strategies with real data.

### Key Features

✅ **Test Strategies on Historical Data** - See how your trading ideas would have performed in the past  
✅ **Compare Multiple Strategies** - Run different approaches and compare results side-by-side  
✅ **Visual Analytics Dashboard** - Interactive charts showing performance, profits/losses, and trade history  
✅ **Detailed Trade Analysis** - Track every entry, exit, profit, and loss with complete trade logs  
✅ **Risk Assessment** - Understand drawdowns, volatility, and other risk metrics  
✅ **Multi-Market Support** - Test strategies across stocks, ETFs, and other instruments  
✅ **No Server Required** - Everything runs locally on your computer — your data stays private  

---

## Quick Start

### 1. Download & Setup
```bash
# Clone the repository
git clone <repository-url>
cd quantfusion

# Build the project
mkdir build && cd build
cmake ..
make
cd ..
```

### 2. Run a Demo Strategy
```bash
./quantfusion 1
```

This runs a built-in demo strategy and generates performance data.

### 3. View Results on the Dashboard
```bash
cd dashboard
python3 -m http.server 8000
```

Then open `http://localhost:8000` in your browser.

---

## The Dashboard: Visualize Your Strategy Performance

![QuantFusion Research Dashboard](img\dashboard_demo.png)

The interactive dashboard shows you everything you need to evaluate your trading strategy:

### Performance Metrics (Left Panel)
- **Total Return** - Overall profit percentage from start to finish
- **CAGR** - Compound Annual Growth Rate (annualized returns)
- **Sharpe Ratio** - Risk-adjusted returns (higher is better)
- **Max Drawdown** - Largest peak-to-trough decline
- **Annual Volatility** - How much returns fluctuate year-to-year

### Main Charts (Center & Right)
- **Equity Curve** (blue line) - Your account value over time
- **Drawdown** (red area) - Periods of losses or underperformance
- **Trade Markers** - Green triangles (entries) and red triangles (exits)
- **Returns Distribution** - Histogram of daily/trade profits
- **Cumulative Returns** - Growth of $1 invested from the start

### Trades Table (Bottom)
A complete log of every trade with:
- Entry and exit prices
- Profit/loss for each trade
- Entry and exit dates/times
- Filter by instrument, strategy, or side (long/short)

---

## How It Works

1. **Define a Strategy** - Choose entry/exit rules, risk parameters, instruments to trade
2. **Backtest on History** - Run the strategy on years of past market data
3. **Analyze Results** - Review the dashboard to see how the strategy performed
4. **Refine & Repeat** - Adjust rules and test again until satisfied

---

## Example Use Cases

🎯 **Test a New Trading Idea** - Validate that a strategy works before risking real money  
📊 **Compare Approaches** - Run 5 different strategies and see which performed best  
💼 **Risk Management** - Understand worst-case scenarios (max drawdown, volatility)  
📈 **Performance Attribution** - See which trades were winners, which were losers  
🔄 **Strategy Optimization** - Test different parameters to find the best settings  

---

## Features Explained Simply

### ✨ Backtesting
Test your trading rules on 5-20+ years of real historical data to see how they would have performed. No guesswork—just hard numbers.

### 📈 Interactive Charts
Zoom, pan, hover over data points to get exact values. Toggle drawdowns and trade markers on/off to focus on what matters.

### 🎯 Multi-Strategy Comparison
Load multiple backtest results and overlay them on the same chart to compare performance directly.

### 📋 Trade Analysis
Click any trade in the table to highlight it on the chart. See entry/exit prices, duration, and P&L for every single trade.

### 🔒 Privacy-First
All data processing happens in your browser. Nothing is uploaded to any server—your strategies and data stay private.

---

## What You'll Learn

By using QuantFusion, you can discover:
- Whether your trading idea is actually profitable
- How risky your strategy is (drawdowns, volatility)
- Which market conditions favor your approach
- How sensitive your strategy is to changing parameters
- Real trade-by-trade performance metrics

---

## Demo Strategies Included

QuantFusion comes with **12 pre-built demo strategies** covering different trading approaches:
- **Moving Average Crossovers** - Trend-following strategies
- **Mean Reversion** - Counter-trend approaches
- **Breakout Strategies** - Trading market breaks
- **Vol-Scaled Approaches** - Adjusting for market volatility

Run each demo to see real backtest results and learn how different strategies perform.

---

## System Requirements

- **Windows, macOS, or Linux**
- **Modern web browser** (Chrome, Firefox, Safari, Edge)
- **Python 3** (for dashboard only)
- **C++17 compiler** (for building - optional if using pre-built executable)

---

## Support & Documentation

- **Technical Docs** - See [ARCHITECTURE.md](./docs/ARCHITECTURE.md) for implementation details
- **Dashboard Guide** - See [dashboard/README.md](./dashboard/README.md) for data format specs
- **Research** - Check [research/](./research/) for alpha factor notebooks and examples

---

## Privacy Notice

✅ QuantFusion operates **100% locally**  
✅ No data transmission to external servers  
✅ No account creation or registration needed  
✅ No analytics or usage tracking  
✅ All computations happen in your browser or on your machine  

---

## Ready to Get Started?

1. Download QuantFusion
2. Run: `./quantfusion 1`
3. Open: `http://localhost:8000`
4. Explore the demo and experiment with your own strategies!

---

## Questions?

For technical questions, see our full [documentation](./docs/).  
For general inquiries, check the [GitHub discussions](https://github.com).

**Happy researching! 📊**
