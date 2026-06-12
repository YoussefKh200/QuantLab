"""
QuantFusion Dashboard — Interactive web-based visualizer
Displays portfolio NAV, risk metrics, strategy signals, and optimization results
"""

import streamlit as st
import pandas as pd
import numpy as np
import plotly.graph_objects as go
import plotly.express as px
from pathlib import Path
import json
from datetime import datetime
import sys

# Configure Streamlit page
st.set_page_config(
    page_title="QuantFusion Dashboard",
    page_icon="📊",
    layout="wide",
    initial_sidebar_state="expanded"
)

# Custom CSS for better styling
st.markdown("""
<style>
    .metric-card {
        background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
        padding: 20px;
        border-radius: 10px;
        color: white;
        margin: 10px 0;
    }
    .metric-value {
        font-size: 32px;
        font-weight: bold;
    }
    .metric-label {
        font-size: 14px;
        opacity: 0.8;
    }
</style>
""", unsafe_allow_html=True)

# ═══════════════════════════════════════════════════════════════════════════
# Data Loading & Management
# ═══════════════════════════════════════════════════════════════════════════

@st.cache_resource
def load_backtest_data():
    """Load backtest results from research_db directory"""
    db_path = Path(__file__).parent.parent / "research_db"
    
    data = {
        "nav_curves": [],
        "trades": [],
        "risk_metrics": {},
        "alphas": [],
        "strategies": []
    }
    
    # Try to load existing data
    try:
        if (db_path / "nav_curves").exists():
            for csv_file in (db_path / "nav_curves").glob("*.csv"):
                data["nav_curves"].append(pd.read_csv(csv_file))
        
        if (db_path / "trades").exists():
            for csv_file in (db_path / "trades").glob("*.csv"):
                data["trades"].append(pd.read_csv(csv_file))
        
        if (db_path / "risk_metrics.json").exists():
            with open(db_path / "risk_metrics.json") as f:
                data["risk_metrics"] = json.load(f)
    
    except Exception as e:
        st.warning(f"Could not load data: {e}")
    
    return data

@st.cache_resource
def load_sample_backtest_data():
    """Generate sample backtest data for demo"""
    np.random.seed(42)
    dates = pd.date_range("2020-01-01", periods=252, freq="D")
    
    # Sample NAV curve
    returns = np.random.normal(0.0005, 0.015, 252)
    nav = 100 * np.exp(np.cumsum(returns))
    
    nav_df = pd.DataFrame({
        "timestamp": dates,
        "nav": nav,
        "equity": nav * 10000,
        "cash": np.ones(252) * 5000,
        "gross_exposure": np.random.uniform(0.8, 1.0, 252),
        "net_exposure": np.random.uniform(-0.2, 0.8, 252)
    })
    
    # Sample trades
    trades_df = pd.DataFrame({
        "timestamp": np.random.choice(dates, 50),
        "symbol": np.random.choice(["SPY", "QQQ", "IWM", "GLD", "TLT"], 50),
        "direction": np.random.choice(["LONG", "SHORT"], 50),
        "entry_price": np.random.uniform(100, 500, 50),
        "exit_price": np.random.uniform(100, 500, 50),
        "pnl": np.random.normal(0, 5000, 50),
        "quantity": np.random.randint(1, 100, 50)
    }).sort_values("timestamp")
    
    return nav_df, trades_df

# ═══════════════════════════════════════════════════════════════════════════
# Header
# ═══════════════════════════════════════════════════════════════════════════

col1, col2, col3 = st.columns([1, 2, 1])
with col1:
    st.image("https://via.placeholder.com/100x100?text=QF", width=80)
with col2:
    st.title("📊 QuantFusion Dashboard")
    st.caption("Real-time backtesting, risk analytics & optimization insights")
with col3:
    st.metric("Last Update", datetime.now().strftime("%H:%M:%S"))

st.divider()

# ═══════════════════════════════════════════════════════════════════════════
# Sidebar — Navigation & Settings
# ═══════════════════════════════════════════════════════════════════════════

with st.sidebar:
    st.header("⚙️ Controls")
    
    dashboard_mode = st.radio(
        "Select View",
        ["📈 Overview", "💼 Portfolio", "⚠️ Risk", "🎯 Signals", "🔬 Optimization", "📊 Analysis"],
        help="Switch between different analysis views"
    )
    
    st.divider()
    
    # Data source selector
    data_source = st.radio(
        "Data Source",
        ["Live (From Research DB)", "Sample Data (Demo)"],
        help="Use live backtest results or demo data"
    )
    
    st.divider()
    
    # Time period selector
    period = st.selectbox(
        "Time Period",
        ["1M", "3M", "6M", "YTD", "1Y", "All"],
        help="Filter data by time period"
    )
    
    # Strategy selector
    strategies = st.multiselect(
        "Strategies",
        ["MA Crossover", "Mean Reversion", "Breakout", "Vol Target", "Cross-Sectional"],
        default=["MA Crossover", "Vol Target"],
        help="Select strategies to display"
    )
    
    st.divider()
    
    # Refresh button
    if st.button("🔄 Refresh Data", use_container_width=True):
        st.cache_resource.clear()
        st.rerun()

# ═══════════════════════════════════════════════════════════════════════════
# Load Data
# ═══════════════════════════════════════════════════════════════════════════

if data_source == "Live (From Research DB)":
    data = load_backtest_data()
    nav_df, trades_df = None, None
else:
    nav_df, trades_df = load_sample_backtest_data()
    data = {"nav_curves": [nav_df] if nav_df is not None else [], "trades": [trades_df] if trades_df is not None else []}

# Use sample data if no live data available
if not nav_df is None:
    pass
else:
    nav_df, trades_df = load_sample_backtest_data()

# ═══════════════════════════════════════════════════════════════════════════
# Dashboard Views
# ═══════════════════════════════════════════════════════════════════════════

if dashboard_mode == "📈 Overview":
    st.header("📈 Performance Overview")
    
    # Key metrics
    col1, col2, col3, col4, col5 = st.columns(5)
    
    total_return = ((nav_df["nav"].iloc[-1] / nav_df["nav"].iloc[0]) - 1) * 100
    sharpe_ratio = nav_df["nav"].pct_change().mean() / nav_df["nav"].pct_change().std() * np.sqrt(252)
    max_dd = ((nav_df["nav"].cummax() - nav_df["nav"]) / nav_df["nav"].cummax()).max() * 100
    win_rate = (nav_df["nav"].pct_change() > 0).mean() * 100
    sortino = nav_df["nav"].pct_change().mean() / nav_df["nav"].pct_change()[nav_df["nav"].pct_change() < 0].std() * np.sqrt(252)
    
    with col1:
        st.metric("Total Return", f"{total_return:.2f}%", f"{total_return/len(nav_df)*252:.2f}% ann")
    with col2:
        st.metric("Sharpe Ratio", f"{sharpe_ratio:.2f}", "Higher is better")
    with col3:
        st.metric("Max Drawdown", f"{max_dd:.2f}%", "Lower is better")
    with col4:
        st.metric("Win Rate", f"{win_rate:.1f}%", "% of profitable days")
    with col5:
        st.metric("Sortino Ratio", f"{sortino:.2f}", "Higher is better")
    
    st.divider()
    
    # NAV curve chart
    st.subheader("Portfolio NAV Curve")
    fig = go.Figure()
    fig.add_trace(go.Scatter(
        x=nav_df["timestamp"],
        y=nav_df["nav"],
        mode="lines",
        name="NAV",
        line=dict(color="#667eea", width=2),
        fill="tozeroy",
        fillcolor="rgba(102, 126, 234, 0.2)"
    ))
    fig.update_layout(
        title="Equity Curve (Starting 100)",
        xaxis_title="Date",
        yaxis_title="NAV",
        hovermode="x unified",
        height=400,
        template="plotly_dark"
    )
    st.plotly_chart(fig, use_container_width=True)
    
    # Drawdown chart
    col1, col2 = st.columns(2)
    
    with col1:
        st.subheader("Drawdown Over Time")
        drawdown = (nav_df["nav"].cummax() - nav_df["nav"]) / nav_df["nav"].cummax() * 100
        fig = go.Figure()
        fig.add_trace(go.Scatter(
            x=nav_df["timestamp"],
            y=drawdown,
            fill="tozeroy",
            fillcolor="rgba(255, 107, 107, 0.3)",
            line=dict(color="rgba(255, 107, 107, 1)"),
            name="Drawdown"
        ))
        fig.update_layout(
            xaxis_title="Date",
            yaxis_title="Drawdown %",
            hovermode="x unified",
            height=300,
            template="plotly_dark"
        )
        st.plotly_chart(fig, use_container_width=True)
    
    with col2:
        st.subheader("Daily Returns Distribution")
        returns = nav_df["nav"].pct_change().dropna() * 100
        fig = px.histogram(returns, nbins=30, title="Distribution of Daily Returns (%)")
        fig.update_layout(height=300, template="plotly_dark", showlegend=False)
        st.plotly_chart(fig, use_container_width=True)

elif dashboard_mode == "💼 Portfolio":
    st.header("💼 Portfolio Analytics")
    
    col1, col2 = st.columns(2)
    
    with col1:
        st.subheader("Exposure Over Time")
        fig = go.Figure()
        fig.add_trace(go.Scatter(
            x=nav_df["timestamp"],
            y=nav_df["gross_exposure"],
            name="Gross Exposure",
            line=dict(color="#667eea")
        ))
        fig.add_trace(go.Scatter(
            x=nav_df["timestamp"],
            y=nav_df["net_exposure"],
            name="Net Exposure",
            line=dict(color="#f093fb")
        ))
        fig.update_layout(
            xaxis_title="Date",
            yaxis_title="Exposure",
            hovermode="x unified",
            height=350,
            template="plotly_dark"
        )
        st.plotly_chart(fig, use_container_width=True)
    
    with col2:
        st.subheader("Equity vs Cash")
        fig = go.Figure()
        fig.add_trace(go.Scatter(
            x=nav_df["timestamp"],
            y=nav_df["equity"],
            name="Equity Value",
            fill="tonexty",
            line=dict(color="#667eea")
        ))
        fig.add_trace(go.Scatter(
            x=nav_df["timestamp"],
            y=nav_df["cash"],
            name="Cash",
            fill="tozeroy",
            line=dict(color="#764ba2")
        ))
        fig.update_layout(
            xaxis_title="Date",
            yaxis_title="Value",
            hovermode="x unified",
            height=350,
            template="plotly_dark"
        )
        st.plotly_chart(fig, use_container_width=True)
    
    # Trades summary
    st.subheader("Trade Summary")
    if not trades_df.empty:
        col1, col2, col3, col4 = st.columns(4)
        
        total_trades = len(trades_df)
        winning_trades = (trades_df["pnl"] > 0).sum()
        losing_trades = (trades_df["pnl"] < 0).sum()
        avg_trade = trades_df["pnl"].mean()
        
        with col1:
            st.metric("Total Trades", total_trades)
        with col2:
            st.metric("Winning Trades", f"{winning_trades} ({winning_trades/total_trades*100:.1f}%)")
        with col3:
            st.metric("Losing Trades", f"{losing_trades} ({losing_trades/total_trades*100:.1f}%)")
        with col4:
            st.metric("Avg Trade P&L", f"${avg_trade:.2f}")
        
        # Top trades
        st.dataframe(
            trades_df.nlargest(10, "pnl")[["timestamp", "symbol", "direction", "entry_price", "exit_price", "pnl"]],
            use_container_width=True
        )

elif dashboard_mode == "⚠️ Risk":
    st.header("⚠️ Risk Metrics & Analysis")
    
    col1, col2, col3 = st.columns(3)
    
    with col1:
        st.metric("Value at Risk (95%)", "$-2,450", "Daily loss at 95% confidence")
    with col2:
        st.metric("Expected Shortfall", "$-3,120", "Average loss beyond VaR")
    with col3:
        st.metric("Volatility", "14.3%", "Annual volatility")
    
    st.divider()
    
    col1, col2 = st.columns(2)
    
    with col1:
        st.subheader("Value at Risk (Rolling 20-day)")
        var_20 = nav_df["nav"].pct_change().rolling(20).quantile(0.05)
        fig = go.Figure()
        fig.add_trace(go.Scatter(
            x=nav_df["timestamp"],
            y=var_20 * 100,
            fill="tozeroy",
            fillcolor="rgba(255, 107, 107, 0.3)",
            line=dict(color="rgba(255, 107, 107, 1)"),
            name="VaR (95%)"
        ))
        fig.update_layout(
            xaxis_title="Date",
            yaxis_title="Daily VaR %",
            hovermode="x unified",
            height=350,
            template="plotly_dark"
        )
        st.plotly_chart(fig, use_container_width=True)
    
    with col2:
        st.subheader("Rolling Volatility (20-day)")
        rolling_vol = nav_df["nav"].pct_change().rolling(20).std() * np.sqrt(252) * 100
        fig = go.Figure()
        fig.add_trace(go.Scatter(
            x=nav_df["timestamp"],
            y=rolling_vol,
            fill="tozeroy",
            fillcolor="rgba(102, 126, 234, 0.3)",
            line=dict(color="#667eea"),
            name="Volatility"
        ))
        fig.update_layout(
            xaxis_title="Date",
            yaxis_title="Annualized Volatility %",
            hovermode="x unified",
            height=350,
            template="plotly_dark"
        )
        st.plotly_chart(fig, use_container_width=True)

elif dashboard_mode == "🎯 Signals":
    st.header("🎯 Strategy Signals & Indicators")
    
    st.info("Strategy signals and alpha indicators will be displayed here once backtests are run.")
    
    # Create sample signal data
    st.subheader("Sample Signal Distribution (MA Crossover)")
    
    signals = np.random.choice([-1, 0, 1], 252, p=[0.2, 0.3, 0.5])
    signal_df = pd.DataFrame({
        "timestamp": nav_df["timestamp"],
        "signal": signals,
        "ma_fast": np.random.uniform(100, 150, 252),
        "ma_slow": np.random.uniform(95, 145, 252),
        "price": np.random.uniform(100, 150, 252)
    })
    
    fig = go.Figure()
    fig.add_trace(go.Scatter(
        x=signal_df["timestamp"],
        y=signal_df["price"],
        name="Price",
        line=dict(color="#667eea", width=2)
    ))
    fig.add_trace(go.Scatter(
        x=signal_df["timestamp"],
        y=signal_df["ma_fast"],
        name="MA(10)",
        line=dict(color="#f093fb", dash="dash")
    ))
    fig.add_trace(go.Scatter(
        x=signal_df["timestamp"],
        y=signal_df["ma_slow"],
        name="MA(50)",
        line=dict(color="#4facfe", dash="dash")
    ))
    fig.update_layout(
        xaxis_title="Date",
        yaxis_title="Price",
        hovermode="x unified",
        height=400,
        template="plotly_dark"
    )
    st.plotly_chart(fig, use_container_width=True)

elif dashboard_mode == "🔬 Optimization":
    st.header("🔬 Hyperparameter Optimization Results")
    
    st.info("Optimization results will be displayed here once optimization runs complete.")
    
    # Create sample optimization data
    params = pd.DataFrame({
        "fast_ma": np.random.randint(5, 50, 30),
        "slow_ma": np.random.randint(50, 200, 30),
        "sharpe_ratio": np.random.uniform(0.5, 2.5, 30)
    })
    
    col1, col2 = st.columns(2)
    
    with col1:
        st.subheader("Parameter Sensitivity Heatmap")
        pivot = params.pivot_table(
            values="sharpe_ratio",
            index="slow_ma",
            columns="fast_ma",
            aggfunc="max"
        )
        fig = px.imshow(pivot, color_continuous_scale="RdYlGn", title="Sharpe Ratio Heatmap")
        fig.update_layout(height=400, template="plotly_dark")
        st.plotly_chart(fig, use_container_width=True)
    
    with col2:
        st.subheader("Top Parameter Sets")
        top_params = params.nlargest(10, "sharpe_ratio")
        st.dataframe(top_params, use_container_width=True)

elif dashboard_mode == "📊 Analysis":
    st.header("📊 Detailed Analysis & Reporting")
    
    tab1, tab2, tab3 = st.tabs(["Performance Stats", "Monthly Returns", "Correlation Matrix"])
    
    with tab1:
        st.subheader("Detailed Performance Statistics")
        
        daily_returns = nav_df["nav"].pct_change().dropna()
        
        stats = {
            "Total Return": f"{(nav_df['nav'].iloc[-1] / nav_df['nav'].iloc[0] - 1) * 100:.2f}%",
            "Annualized Return": f"{daily_returns.mean() * 252 * 100:.2f}%",
            "Annualized Volatility": f"{daily_returns.std() * np.sqrt(252) * 100:.2f}%",
            "Sharpe Ratio": f"{(daily_returns.mean() / daily_returns.std() * np.sqrt(252)):.2f}",
            "Sortino Ratio": f"{(daily_returns.mean() / daily_returns[daily_returns < 0].std() * np.sqrt(252)):.2f}",
            "Max Drawdown": f"{((nav_df['nav'].cummax() - nav_df['nav']) / nav_df['nav'].cummax()).max() * 100:.2f}%",
            "Win Rate": f"{(daily_returns > 0).mean() * 100:.2f}%",
            "Profit Factor": f"{daily_returns[daily_returns > 0].sum() / abs(daily_returns[daily_returns < 0].sum()):.2f}",
        }
        
        stats_df = pd.DataFrame(list(stats.items()), columns=["Metric", "Value"])
        st.table(stats_df)
    
    with tab2:
        st.subheader("Monthly Returns Heatmap")
        nav_df["date"] = pd.to_datetime(nav_df["timestamp"])
        nav_df["year"] = nav_df["date"].dt.year
        nav_df["month"] = nav_df["date"].dt.month
        monthly_returns = nav_df.groupby(["year", "month"])["nav"].apply(
            lambda x: (x.iloc[-1] / x.iloc[0] - 1) * 100 if len(x) > 1 else 0
        ).unstack(fill_value=0)
        
        fig = px.imshow(monthly_returns, color_continuous_scale="RdYlGn", text_auto=".1f")
        fig.update_layout(height=300, template="plotly_dark")
        st.plotly_chart(fig, use_container_width=True)
    
    with tab3:
        st.subheader("Correlation Matrix")
        # Create sample corr matrix for visualization
        corr_data = np.random.uniform(-1, 1, (5, 5))
        corr_data = (corr_data + corr_data.T) / 2  # Make symmetric
        np.fill_diagonal(corr_data, 1)
        
        fig = px.imshow(
            corr_data,
            labels=dict(x="Asset", y="Asset", color="Correlation"),
            x=["SPY", "QQQ", "IWM", "GLD", "TLT"],
            y=["SPY", "QQQ", "IWM", "GLD", "TLT"],
            color_continuous_scale="RdBu",
            zmin=-1,
            zmax=1
        )
        fig.update_layout(height=400, template="plotly_dark")
        st.plotly_chart(fig, use_container_width=True)

# ═══════════════════════════════════════════════════════════════════════════
# Footer
# ═══════════════════════════════════════════════════════════════════════════

st.divider()
col1, col2, col3 = st.columns(3)
with col1:
    st.caption("QuantFusion Dashboard v1.0")
with col2:
    st.caption(f"Data updated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
with col3:
    st.caption("[Documentation](https://github.com) | [Support](mailto:support@quantfusion.io)")
