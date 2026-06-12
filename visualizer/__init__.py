"""
Integration module to connect C++ backtest results to dashboard
This file shows how to export data from your C++ Backtester to the visualizer
"""

# Example usage in C++ (via PyBridge):
#
# from visualizer.data_export import QuantFusionDataExporter
# import numpy as np
#
# exporter = QuantFusionDataExporter("research_db")
#
# # After running backtest, export results:
# exporter.export_nav_curve(
#     timestamps=backtest_results.timestamps,
#     nav_values=backtest_results.nav,
#     equity=backtest_results.equity,
#     cash=backtest_results.cash,
#     gross_exposure=backtest_results.gross_exposure,
#     net_exposure=backtest_results.net_exposure,
#     strategy_name="ma_crossover_v1"
# )
#
# exporter.export_trades(
#     trades=backtest_results.trades,
#     strategy_name="ma_crossover_v1"
# )
#
# exporter.export_risk_metrics({
#     "var_95": backtest_results.var_95,
#     "max_drawdown": backtest_results.max_drawdown,
#     "sharpe_ratio": backtest_results.sharpe_ratio,
#     ...
# })

from data_export import QuantFusionDataExporter, QuantFusionDataLoader

__all__ = ["QuantFusionDataExporter", "QuantFusionDataLoader"]
