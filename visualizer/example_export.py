"""
Example: Exporting QuantFusion Backtest Results to Dashboard
Demonstrates the complete flow from C++ backtest → CSV/JSON → Web Dashboard

This script shows how to integrate the visualizer with your PyBridge C++ backend
"""

import sys
from pathlib import Path
import numpy as np
from datetime import datetime, timedelta

# Add visualizer to path
sys.path.insert(0, str(Path(__file__).parent))

from data_export import QuantFusionDataExporter, QuantFusionDataLoader

def generate_sample_backtest_data():
    """Generate realistic sample backtest data for demonstration"""
    np.random.seed(42)
    
    # Generate 252 trading days (~1 year)
    num_days = 252
    start_date = datetime(2023, 1, 1)
    dates = [start_date + timedelta(days=i) for i in range(num_days)]
    timestamps = [d.timestamp() for d in dates]
    
    # Simulate realistic portfolio evolution
    daily_returns = np.random.normal(0.0005, 0.012, num_days)  # 0.05% avg, 1.2% vol
    nav_values = 100 * np.exp(np.cumsum(daily_returns))
    
    # Simulate equity and cash allocation
    initial_capital = 1_000_000
    equity_values = nav_values * (initial_capital / 100)
    cash_values = initial_capital * 0.1 * np.ones(num_days)  # 10% cash buffer
    
    # Simulate exposures
    gross_exposure = np.random.uniform(0.8, 1.0, num_days)
    net_exposure = np.random.uniform(-0.2, 0.8, num_days)
    
    return {
        "timestamps": timestamps,
        "dates": dates,
        "nav": nav_values.tolist(),
        "equity": equity_values.tolist(),
        "cash": cash_values.tolist(),
        "gross_exposure": gross_exposure.tolist(),
        "net_exposure": net_exposure.tolist()
    }

def generate_sample_trades(dates: list):
    """Generate realistic sample trade data"""
    np.random.seed(42)
    num_trades = 50
    
    symbols = ["SPY", "QQQ", "IWM", "GLD", "TLT"]
    directions = ["LONG", "SHORT"]
    
    trades = []
    for i in range(num_trades):
        # Random trade on random date
        trade_date = np.random.choice(dates)
        symbol = np.random.choice(symbols)
        direction = np.random.choice(directions)
        
        entry_price = np.random.uniform(100, 500)
        exit_price = entry_price * np.random.uniform(0.98, 1.02)
        
        if direction == "LONG":
            pnl = (exit_price - entry_price) * 100
        else:
            pnl = (entry_price - exit_price) * 100
        
        trades.append({
            "timestamp": trade_date.timestamp(),
            "symbol": symbol,
            "direction": direction,
            "entry_price": entry_price,
            "exit_price": exit_price,
            "pnl": pnl,
            "quantity": np.random.randint(10, 200)
        })
    
    return sorted(trades, key=lambda x: x["timestamp"])

def main():
    """
    Main example: Export backtest results to dashboard format
    
    In real usage, you would:
    1. Run your C++ Backtester via PyBridge
    2. Get the backtest results
    3. Use QuantFusionDataExporter to save them
    4. Open the Streamlit dashboard to visualize
    """
    
    print("\n" + "="*70)
    print("QuantFusion Dashboard — Data Export Example")
    print("="*70 + "\n")
    
    # Step 1: Generate sample backtest data (replace with real C++ results)
    print("📊 Generating sample backtest data...")
    backtest_data = generate_sample_backtest_data()
    trades = generate_sample_trades(backtest_data["dates"])
    print(f"   ✓ Generated {len(backtest_data['nav'])} days of backtest data")
    print(f"   ✓ Generated {len(trades)} trades")
    
    # Step 2: Initialize data exporter
    print("\n💾 Exporting data to research_db/...")
    exporter = QuantFusionDataExporter("research_db")
    
    # Step 3: Export NAV curve
    nav_file = exporter.export_nav_curve(
        timestamps=backtest_data["timestamps"],
        nav_values=backtest_data["nav"],
        equity=backtest_data["equity"],
        cash=backtest_data["cash"],
        gross_exposure=backtest_data["gross_exposure"],
        net_exposure=backtest_data["net_exposure"],
        strategy_name="ma_crossover_example"
    )
    print(f"   ✓ NAV curve exported to: {nav_file}")
    
    # Step 4: Export trades
    trades_file = exporter.export_trades(
        trades=trades,
        strategy_name="ma_crossover_example"
    )
    print(f"   ✓ Trades exported to: {trades_file}")
    
    # Step 5: Export risk metrics
    nav_array = np.array(backtest_data["nav"])
    daily_returns = np.diff(nav_array) / nav_array[:-1]
    
    risk_metrics = {
        "sharpe_ratio": float((np.mean(daily_returns) / np.std(daily_returns)) * np.sqrt(252)),
        "sortino_ratio": float((np.mean(daily_returns) / np.std(daily_returns[daily_returns < 0])) * np.sqrt(252)) if np.any(daily_returns < 0) else np.inf,
        "max_drawdown_pct": float(np.max((np.maximum.accumulate(nav_array) - nav_array) / np.maximum.accumulate(nav_array)) * 100),
        "var_95_daily_pct": float(np.percentile(daily_returns, 5) * 100),
        "cvar_95_daily_pct": float(np.mean(daily_returns[daily_returns <= np.percentile(daily_returns, 5)]) * 100),
        "win_rate_pct": float(np.mean(daily_returns > 0) * 100),
        "total_return_pct": float((nav_array[-1] / nav_array[0] - 1) * 100),
        "annual_volatility_pct": float(np.std(daily_returns) * np.sqrt(252) * 100),
        "num_days": len(backtest_data["nav"]),
        "start_date": backtest_data["dates"][0].isoformat(),
        "end_date": backtest_data["dates"][-1].isoformat()
    }
    
    metrics_file = exporter.export_risk_metrics(risk_metrics)
    print(f"   ✓ Risk metrics exported to: {metrics_file}")
    
    # Step 6: Export alpha signals (example)
    signal_data = {
        "ma_fast": np.random.uniform(100, 150, len(backtest_data["timestamps"])).tolist(),
        "ma_slow": np.random.uniform(95, 145, len(backtest_data["timestamps"])).tolist(),
        "signal": np.random.choice([-1, 0, 1], len(backtest_data["timestamps"])).tolist(),
    }
    
    signals_file = exporter.export_alpha_signals(
        timestamps=backtest_data["timestamps"],
        signals=signal_data,
        strategy_name="ma_crossover_example"
    )
    print(f"   ✓ Alpha signals exported to: {signals_file}")
    
    # Step 7: Load and verify
    print("\n✅ Verifying exported data...")
    loader = QuantFusionDataLoader("research_db")
    
    nav_curves = loader.load_nav_curves()
    trades_list = loader.load_trades()
    metrics = loader.load_risk_metrics()
    signals = loader.load_alpha_signals()
    
    print(f"   ✓ Loaded {len(nav_curves)} NAV curve(s)")
    print(f"   ✓ Loaded {len(trades_list)} trade file(s)")
    print(f"   ✓ Loaded {len(signals)} signal file(s)")
    
    # Print performance summary
    if nav_curves:
        summary = loader.get_strategy_summary(nav_curves[0])
        print(f"\n📈 Performance Summary:")
        print(f"   Total Return: {summary['total_return_pct']:.2f}%")
        print(f"   Annualized Return: {summary['annualized_return_pct']:.2f}%")
        print(f"   Sharpe Ratio: {summary['sharpe_ratio']:.2f}")
        print(f"   Max Drawdown: {summary['max_drawdown_pct']:.2f}%")
        print(f"   Win Rate: {summary['win_rate_pct']:.1f}%")
    
    print("\n" + "="*70)
    print("✨ Data export complete!")
    print("\nNext steps:")
    print("1. Start the dashboard: streamlit run visualizer/dashboard.py")
    print("2. Open http://localhost:8501 in your browser")
    print("3. Explore your backtest results interactively")
    print("="*70 + "\n")

if __name__ == "__main__":
    main()
