# QuantFusion Research Dashboard

Standalone, client-side NAV / trade analytics viewer. No build step,
no server required — open `index.html` in a browser.

## Quick start

```bash
# From the quantfusion/ root, after running a backtest that has
# called bt.export_nav_csv(...) / bt.export_trades_csv(...) or
# storage.persist_nav_curve(...) / persist_trades(...):

open dashboard/index.html        # macOS
xdg-open dashboard/index.html    # Linux
# or just double-click it in a file browser
```

If your browser blocks local CSV drag-and-drop for `file://` pages,
serve the folder instead:

```bash
cd dashboard && python -m http.server 8000
# then visit http://localhost:8000
```

## Generating data from QuantFusion

### Option A — Backtester direct export

```cpp
ql::Backtester bt(cfg);
bt.add_feed(feed);
bt.add_strategy(std::make_shared<ql::MACrossStrategy>(params), "ma_cross");
bt.run();

bt.export_nav_csv("dashboard/data/nav.csv");
bt.export_trades_csv("dashboard/data/trades.csv");
```

### Option B — StorageEngine (per-strategy, supports many strategies)

```cpp
ql::storage::FlatFileStorage storage("dashboard/data");
storage.persist_nav_curve(bt.portfolio().nav_curve(), "ma_cross");
storage.persist_trades(bt.portfolio().trades(), "ma_cross");
```

This writes `dashboard/data/nav_curves/ma_cross.csv` and
`dashboard/data/trades/ma_cross.csv`. Drag those files onto the
dashboard's NAV / Trades drop zones (or use the file picker — multiple
NAV files can be loaded together to overlay equity curves from
different strategies/runs).

## What's in dashboard/data/

A sample real run (Demo 1: vol-scaled MA crossover across
SPY/QQQ/IWM/GLD/TLT, 2019-2023, 1260 bars, 78 trades) is included as
`nav.csv` and `trades.csv` so you can open the dashboard and see real
output immediately without running anything. Regenerate with:

```bash
cd ..  # quantfusion/ root
./quantfusion 1
cp /tmp/qf_nav_demo1.csv    dashboard/data/nav.csv
cp /tmp/qf_trades_demo1.csv dashboard/data/trades.csv
```

## Features

- Interactive equity curve with drawdown overlay (Plotly: zoom, pan,
  hover tooltips, legend toggle)
- Trade entry (green ▲) / exit (red ▼) markers on the NAV chart, with
  full trade details on hover
- Multiple NAV file overlay (compare strategies/runs side by side)
- Summary panel: Total Return, CAGR, Annualized Vol, Sharpe, Max
  Drawdown, Total Trades — all computed client-side
- Sortable/searchable/paginated trades table with instrument,
  strategy, side, and date-range filters
- Row click → highlights the trade's entry/exit on the NAV chart
- PnL histogram / daily returns histogram toggle
- Cumulative returns chart (linear or log)
- Export filtered trades to CSV, export any chart to PNG

## Notes

- Timestamps: QuantFusion's `Timestamp` is nanoseconds since epoch —
  the dashboard's parser auto-detects this by magnitude (values
  `> 1e17` are treated as nanoseconds). ISO-8601 strings also work.
- `drawdown` is computed client-side from `nav` if the CSV doesn't
  include that column (both `Backtester::export_nav_csv` and
  `FlatFileStorage::persist_nav_curve` already include it).
- Annualization (Sharpe, CAGR, vol) infers bars-per-year from the
  median timestamp gap in the NAV series — works for daily, hourly,
  or any other regular bar frequency without configuration.
- All processing happens in-browser. No data is uploaded anywhere.
