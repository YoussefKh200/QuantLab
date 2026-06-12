"""
Data export utilities for QuantFusion C++ backend → Dashboard
Handles conversion of backtest results to Streamlit-compatible formats
"""

import json
import pandas as pd
from pathlib import Path
from typing import Dict, List, Optional
import numpy as np
from datetime import datetime

class QuantFusionDataExporter:
    """Export C++ backtest results to JSON/CSV for visualization"""
    
    def __init__(self, output_dir: str = "research_db"):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        # Create subdirectories
        for subdir in ["nav_curves", "trades", "risk_metrics", "alphas", "strategies"]:
            (self.output_dir / subdir).mkdir(exist_ok=True)
    
    def export_nav_curve(self, timestamps: List[float], nav_values: List[float], 
                        equity: List[float], cash: List[float],
                        gross_exposure: List[float], net_exposure: List[float],
                        strategy_name: str = "strategy_1"):
        """Export portfolio NAV curve and exposures"""
        df = pd.DataFrame({
            "timestamp": pd.to_datetime(timestamps, unit='s'),
            "nav": nav_values,
            "equity": equity,
            "cash": cash,
            "gross_exposure": gross_exposure,
            "net_exposure": net_exposure
        })
        
        output_file = self.output_dir / "nav_curves" / f"{strategy_name}_nav.csv"
        df.to_csv(output_file, index=False)
        return output_file
    
    def export_trades(self, trades: List[Dict], strategy_name: str = "strategy_1"):
        """Export trade records"""
        df = pd.DataFrame(trades)
        if "timestamp" in df.columns:
            df["timestamp"] = pd.to_datetime(df["timestamp"], unit='s')
        
        output_file = self.output_dir / "trades" / f"{strategy_name}_trades.csv"
        df.to_csv(output_file, index=False)
        return output_file
    
    def export_risk_metrics(self, metrics: Dict):
        """Export risk metrics summary"""
        output_file = self.output_dir / "risk_metrics.json"
        
        # Parse nested structures
        cleaned_metrics = self._clean_for_json(metrics)
        
        with open(output_file, 'w') as f:
            json.dump(cleaned_metrics, f, indent=2)
        
        return output_file
    
    def export_alpha_signals(self, timestamps: List[float], signals: Dict[str, List[float]],
                            strategy_name: str = "strategy_1"):
        """Export alpha signals and indicators"""
        df = pd.DataFrame({
            "timestamp": pd.to_datetime(timestamps, unit='s'),
            **signals
        })
        
        output_file = self.output_dir / "alphas" / f"{strategy_name}_signals.csv"
        df.to_csv(output_file, index=False)
        return output_file
    
    def export_optimization_results(self, param_sets: List[Dict], 
                                   fitness_scores: List[float],
                                   best_params: Dict,
                                   optimization_name: str = "optimization_1"):
        """Export hyperparameter optimization results"""
        results = {
            "param_sets": param_sets,
            "fitness_scores": fitness_scores,
            "best_params": best_params,
            "timestamp": datetime.now().isoformat()
        }
        
        output_file = self.output_dir / f"{optimization_name}_results.json"
        
        with open(output_file, 'w') as f:
            json.dump(results, f, indent=2)
        
        return output_file
    
    def export_strategy_metadata(self, strategy_name: str, metadata: Dict):
        """Export strategy metadata and configuration"""
        output_file = self.output_dir / "strategies" / f"{strategy_name}_metadata.json"
        
        cleaned_metadata = self._clean_for_json(metadata)
        
        with open(output_file, 'w') as f:
            json.dump(cleaned_metadata, f, indent=2)
        
        return output_file
    
    @staticmethod
    def _clean_for_json(obj):
        """Convert numpy/non-serializable types to JSON-compatible"""
        if isinstance(obj, dict):
            return {k: QuantFusionDataExporter._clean_for_json(v) for k, v in obj.items()}
        elif isinstance(obj, (list, tuple)):
            return [QuantFusionDataExporter._clean_for_json(item) for item in obj]
        elif isinstance(obj, np.ndarray):
            return obj.tolist()
        elif isinstance(obj, (np.integer, np.floating)):
            return float(obj)
        elif isinstance(obj, (float, int, str, bool, type(None))):
            return obj
        else:
            return str(obj)


class QuantFusionDataLoader:
    """Load backtest results from research_db for visualization"""
    
    def __init__(self, db_dir: str = "research_db"):
        self.db_dir = Path(db_dir)
    
    def load_nav_curves(self) -> List[pd.DataFrame]:
        """Load all NAV curve files"""
        curves = []
        nav_dir = self.db_dir / "nav_curves"
        
        if nav_dir.exists():
            for csv_file in nav_dir.glob("*.csv"):
                df = pd.read_csv(csv_file)
                df["timestamp"] = pd.to_datetime(df["timestamp"])
                curves.append(df)
        
        return curves
    
    def load_trades(self) -> List[pd.DataFrame]:
        """Load all trade files"""
        trades = []
        trades_dir = self.db_dir / "trades"
        
        if trades_dir.exists():
            for csv_file in trades_dir.glob("*.csv"):
                df = pd.read_csv(csv_file)
                df["timestamp"] = pd.to_datetime(df["timestamp"])
                trades.append(df)
        
        return trades
    
    def load_risk_metrics(self) -> Dict:
        """Load risk metrics"""
        metrics_file = self.db_dir / "risk_metrics.json"
        
        if metrics_file.exists():
            with open(metrics_file) as f:
                return json.load(f)
        
        return {}
    
    def load_alpha_signals(self) -> List[pd.DataFrame]:
        """Load all alpha signal files"""
        signals = []
        alphas_dir = self.db_dir / "alphas"
        
        if alphas_dir.exists():
            for csv_file in alphas_dir.glob("*.csv"):
                df = pd.read_csv(csv_file)
                df["timestamp"] = pd.to_datetime(df["timestamp"])
                signals.append(df)
        
        return signals
    
    def load_optimization_results(self) -> List[Dict]:
        """Load all optimization results"""
        results = []
        
        for json_file in self.db_dir.glob("*_results.json"):
            with open(json_file) as f:
                results.append(json.load(f))
        
        return results
    
    def get_strategy_summary(self, nav_df: pd.DataFrame) -> Dict:
        """Calculate performance summary from NAV curve"""
        returns = nav_df["nav"].pct_change().dropna()
        
        return {
            "total_return_pct": (nav_df["nav"].iloc[-1] / nav_df["nav"].iloc[0] - 1) * 100,
            "annualized_return_pct": returns.mean() * 252 * 100,
            "annualized_volatility_pct": returns.std() * np.sqrt(252) * 100,
            "sharpe_ratio": (returns.mean() / returns.std() * np.sqrt(252)),
            "sortino_ratio": (returns.mean() / returns[returns < 0].std() * np.sqrt(252)) if (returns < 0).any() else np.inf,
            "max_drawdown_pct": ((nav_df["nav"].cummax() - nav_df["nav"]) / nav_df["nav"].cummax()).max() * 100,
            "win_rate_pct": (returns > 0).mean() * 100,
            "num_days": len(nav_df),
            "start_date": nav_df["timestamp"].iloc[0],
            "end_date": nav_df["timestamp"].iloc[-1]
        }
