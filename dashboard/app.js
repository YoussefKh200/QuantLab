/* ============================================================================
   QuantFusion Research Dashboard — Application Logic
   ----------------------------------------------------------------------
   Sections:
     1. Constants & global state
     2. Embedded demo data
     3. Utility functions (timestamp parsing, formatting, stats)
     4. CSV loading (drag/drop + file input)
     5. Performance metrics computation
     6. Chart rendering (NAV/drawdown, returns distribution, cumulative)
     7. Trades table (sort/filter/search/paginate/export)
     8. Wiring / event listeners
   ============================================================================ */

/* ===================== 1. CONSTANTS & GLOBAL STATE ======================= */

const RISK_FREE_RATE = 0.0;          // annual, used in Sharpe calc
const DEFAULT_BARS_PER_YEAR = 252;   // fallback annualisation factor

const SERIES_COLORS = [
  "#2563eb", "#15803d", "#b45309", "#9333ea", "#0891b2",
  "#dc2626", "#65a30d", "#c026d3", "#0284c7", "#ca8a04"
];

// Global application state
const state = {
  navSeries: [],     // array of { name, color, rows: [{ts, nav, cash, drawdown}] }
  trades: [],        // array of normalized trade objects
  tradesRaw: [],     // unfiltered, normalized
  filtered: [],      // filtered+sorted trades currently shown
  sortKey: "exit_ts",
  sortDir: "desc",
  page: 1,
  pageSize: 25,
  selectedTradeIdx: null,
  isDemo: true,
};

/* ===================== 2. EMBEDDED DEMO DATA ============================== */
// A small synthetic dataset so the dashboard is functional with zero files.

const DEMO_NAV_CSV = `timestamp,nav,cash,drawdown
2024-01-02,1000000,1000000,0
2024-01-03,1004200,800000,0
2024-01-04,1001800,800000,0.0024
2024-01-05,1012500,750000,0
2024-01-08,1018700,750000,0
2024-01-09,1009300,700000,0.0092
2024-01-10,1022100,700000,0
2024-01-11,1031800,650000,0
2024-01-12,1027400,650000,0.0043
2024-01-15,1041200,600000,0
2024-01-16,1038900,600000,0.0022
2024-01-17,1052300,600000,0
2024-01-18,1049700,550000,0.0025
2024-01-19,1063800,550000,0
2024-01-22,1058100,550000,0.0054
2024-01-23,1071400,500000,0
2024-01-24,1066900,500000,0.0042
2024-01-25,1082600,500000,0
2024-01-26,1078300,450000,0.0040
2024-01-29,1095100,450000,0
2024-01-30,1089200,450000,0.0054
2024-01-31,1102700,400000,0
2024-02-01,1098400,400000,0.0039
2024-02-02,1112800,400000,0
2024-02-05,1107500,400000,0.0048
2024-02-06,1121300,350000,0
2024-02-07,1117100,350000,0.0037
2024-02-08,1130600,350000,0
2024-02-09,1125200,300000,0.0048
2024-02-12,1139400,300000,0
`;

const DEMO_TRADES_CSV = `instrument,strategy_id,side,qty,entry_price,exit_price,entry_ts,exit_ts,pnl,commission,slippage
AAPL,ma_cross_10_50,LONG,500,182.10,185.40,2024-01-03,2024-01-08,1650.00,5.00,12.30
MSFT,ma_cross_10_50,LONG,300,371.20,374.80,2024-01-04,2024-01-10,1080.00,3.00,8.10
SPY,mean_rev_20,SHORT,200,478.50,475.10,2024-01-09,2024-01-12,680.00,2.00,5.40
AAPL,ma_cross_10_50,LONG,500,186.90,184.20,2024-01-16,2024-01-18,-1350.00,5.00,11.80
NVDA,breakout_20,LONG,100,548.30,562.10,2024-01-17,2024-01-24,1380.00,1.50,9.20
MSFT,ma_cross_10_50,LONG,300,376.40,372.90,2024-01-22,2024-01-26,-1050.00,3.00,7.60
SPY,mean_rev_20,SHORT,200,481.20,474.60,2024-01-25,2024-01-31,1320.00,2.00,6.10
NVDA,breakout_20,LONG,100,565.00,571.40,2024-01-30,2024-02-05,640.00,1.50,8.90
AAPL,ma_cross_10_50,LONG,500,183.40,187.10,2024-02-01,2024-02-07,1850.00,5.00,10.50
MSFT,ma_cross_10_50,LONG,300,374.10,378.60,2024-02-06,2024-02-12,1350.00,3.00,7.20
`;

/* ===================== 3. UTILITY FUNCTIONS =============================== */

/**
 * Parse a timestamp value that may be:
 *  - a number (UNIX seconds, milliseconds, or nanoseconds — auto-detected
 *    by magnitude)
 *  - a string parseable by Date() (ISO-8601, "YYYY-MM-DD", etc.)
 * Returns a JS Date object, or null if unparseable.
 */
function parseTimestamp(val) {
  if (val === null || val === undefined || val === "") return null;

  // Numeric?
  const num = Number(val);
  if (!Number.isNaN(num) && /^-?\d+(\.\d+)?$/.test(String(val).trim())) {
    let ms;
    if (num > 1e17) {
      ms = num / 1e6;        // nanoseconds -> ms
    } else if (num > 1e14) {
      ms = num / 1e3;        // microseconds -> ms
    } else if (num > 1e11) {
      ms = num;              // already milliseconds
    } else if (num > 1e8) {
      ms = num * 1000;       // seconds -> ms
    } else {
      ms = num;              // fallback: assume ms (small numbers, e.g. bar index)
    }
    const d = new Date(ms);
    return Number.isNaN(d.getTime()) ? null : d;
  }

  // String date
  const d = new Date(val);
  return Number.isNaN(d.getTime()) ? null : d;
}

/** Format a number as currency-like string with thousands separators. */
function fmtNumber(v, decimals = 2) {
  if (v === null || v === undefined || Number.isNaN(v)) return "—";
  return Number(v).toLocaleString("en-US", {
    minimumFractionDigits: decimals,
    maximumFractionDigits: decimals,
  });
}

/** Format a fraction as a percentage string, e.g. 0.0523 -> "5.23%" */
function fmtPct(v, decimals = 2) {
  if (v === null || v === undefined || Number.isNaN(v)) return "—";
  return (v * 100).toFixed(decimals) + "%";
}

/** Format a Date as YYYY-MM-DD HH:MM for table display. */
function fmtDate(d) {
  if (!d) return "—";
  const pad = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
         `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/** Format a Date as YYYY-MM-DD for <input type="date"> comparisons. */
function fmtDateOnly(d) {
  if (!d) return "";
  const pad = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

/**
 * Infer the number of bars per year from a sorted array of Date objects,
 * using the median gap between consecutive timestamps. Falls back to
 * DEFAULT_BARS_PER_YEAR (252) if there's not enough data.
 */
function inferBarsPerYear(dates) {
  if (dates.length < 3) return DEFAULT_BARS_PER_YEAR;
  const gaps = [];
  for (let i = 1; i < dates.length; i++) {
    gaps.push(dates[i].getTime() - dates[i - 1].getTime());
  }
  gaps.sort((a, b) => a - b);
  const medianMs = gaps[Math.floor(gaps.length / 2)];
  if (medianMs <= 0) return DEFAULT_BARS_PER_YEAR;
  const msPerYear = 365.25 * 24 * 3600 * 1000;
  const inferred = Math.round(msPerYear / medianMs);
  // Clamp to sane bounds
  if (inferred < 1) return 1;
  if (inferred > 252 * 24) return 252 * 24; // cap at hourly
  return inferred;
}

/** Mean of an array of numbers. */
function mean(arr) {
  if (arr.length === 0) return 0;
  return arr.reduce((a, b) => a + b, 0) / arr.length;
}

/** Sample standard deviation (n-1 denominator). */
function stdev(arr) {
  if (arr.length < 2) return 0;
  const m = mean(arr);
  const variance = arr.reduce((a, b) => a + (b - m) ** 2, 0) / (arr.length - 1);
  return Math.sqrt(variance);
}

/** Escape a value for CSV output. */
function csvEscape(val) {
  if (val === null || val === undefined) return "";
  const s = String(val);
  if (s.includes(",") || s.includes('"') || s.includes("\n")) {
    return '"' + s.replace(/"/g, '""') + '"';
  }
  return s;
}

/* ===================== 4. CSV LOADING ===================================== */

/**
 * Parse a NAV CSV file's text content into a normalized series object.
 * Expected columns (case-insensitive): timestamp, nav, cash, drawdown
 */
function parseNavCsv(text, fileName) {
  const parsed = Papa.parse(text, {
    header: true,
    skipEmptyLines: true,
    dynamicTyping: false,
  });

  const cols = parsed.meta.fields.map((f) => f.trim().toLowerCase());
  const colMap = {};
  parsed.meta.fields.forEach((f) => { colMap[f.trim().toLowerCase()] = f; });

  if (!cols.includes("timestamp") || !cols.includes("nav")) {
    throw new Error(`"${fileName}": expected columns "timestamp" and "nav" not found.`);
  }

  const rows = [];
  for (const row of parsed.data) {
    const ts = parseTimestamp(row[colMap["timestamp"]]);
    const nav = Number(row[colMap["nav"]]);
    if (!ts || Number.isNaN(nav)) continue;

    const cash = colMap["cash"] !== undefined ? Number(row[colMap["cash"]]) : null;
    let drawdown = colMap["drawdown"] !== undefined
      ? Number(row[colMap["drawdown"]])
      : null;

    rows.push({ ts, nav, cash, drawdown: Number.isNaN(drawdown) ? null : drawdown });
  }

  rows.sort((a, b) => a.ts - b.ts);

  // If drawdown column missing/empty, compute it from the NAV series
  const hasDrawdown = rows.some((r) => r.drawdown !== null && r.drawdown !== undefined);
  if (!hasDrawdown && rows.length > 0) {
    let peak = rows[0].nav;
    for (const r of rows) {
      peak = Math.max(peak, r.nav);
      r.drawdown = peak > 0 ? (peak - r.nav) / peak : 0;
    }
  }

  return { name: fileName.replace(/\.csv$/i, ""), rows };
}

/**
 * Parse a Trades CSV file's text content into normalized trade objects.
 */
function parseTradesCsv(text) {
  const parsed = Papa.parse(text, {
    header: true,
    skipEmptyLines: true,
    dynamicTyping: false,
  });

  const colMap = {};
  parsed.meta.fields.forEach((f) => { colMap[f.trim().toLowerCase()] = f; });

  const get = (row, key) => colMap[key] !== undefined ? row[colMap[key]] : undefined;

  const trades = [];
  for (const row of parsed.data) {
    const entry_ts = parseTimestamp(get(row, "entry_ts"));
    const exit_ts = parseTimestamp(get(row, "exit_ts"));
    const pnl = Number(get(row, "pnl"));
    if (!entry_ts && !exit_ts && Number.isNaN(pnl)) continue; // skip junk rows

    let side = (get(row, "side") || "").toString().trim().toUpperCase();
    if (side === "BUY") side = "LONG";
    if (side === "SELL") side = "SHORT";

    trades.push({
      instrument: (get(row, "instrument") || "").toString(),
      strategy_id: (get(row, "strategy_id") || "").toString(),
      side: side || "—",
      qty: Number(get(row, "qty")) || 0,
      entry_price: Number(get(row, "entry_price")) || 0,
      exit_price: Number(get(row, "exit_price")) || 0,
      entry_ts,
      exit_ts,
      pnl: Number.isNaN(pnl) ? 0 : pnl,
      commission: Number(get(row, "commission")) || 0,
      slippage: Number(get(row, "slippage")) || 0,
    });
  }

  trades.sort((a, b) => {
    const ea = a.exit_ts ? a.exit_ts.getTime() : 0;
    const eb = b.exit_ts ? b.exit_ts.getTime() : 0;
    return ea - eb;
  });

  return trades;
}

/** Read a File object as text (Promise-based). */
function readFileAsText(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = (e) => resolve(e.target.result);
    reader.onerror = (e) => reject(e);
    reader.readAsText(file);
  });
}

/* ===================== 5. PERFORMANCE METRICS ============================= */

/**
 * Compute summary performance metrics from a NAV series (array of
 * {ts, nav, cash, drawdown}, sorted ascending by ts).
 * Returns an object with: totalReturn, cagr, annVol, sharpe, maxDrawdown,
 * finalNav, initialNav, numBars, barsPerYear.
 */
function computeMetrics(rows) {
  if (!rows || rows.length < 2) {
    return {
      totalReturn: null, cagr: null, annVol: null, sharpe: null,
      maxDrawdown: null, finalNav: rows?.[0]?.nav ?? null,
      initialNav: rows?.[0]?.nav ?? null, numBars: rows?.length ?? 0,
      barsPerYear: DEFAULT_BARS_PER_YEAR,
    };
  }

  const initialNav = rows[0].nav;
  const finalNav = rows[rows.length - 1].nav;
  const totalReturn = initialNav !== 0 ? (finalNav / initialNav) - 1 : null;

  const dates = rows.map((r) => r.ts);
  const barsPerYear = inferBarsPerYear(dates);

  const years = (dates[dates.length - 1].getTime() - dates[0].getTime())
    / (365.25 * 24 * 3600 * 1000);

  const cagr = (years > 0 && initialNav > 0 && finalNav > 0)
    ? Math.pow(finalNav / initialNav, 1 / years) - 1
    : null;

  // Per-bar simple returns
  const rets = [];
  for (let i = 1; i < rows.length; i++) {
    const prev = rows[i - 1].nav;
    if (prev !== 0) rets.push((rows[i].nav - prev) / prev);
  }

  const meanRet = mean(rets);
  const sdRet = stdev(rets);
  const annVol = sdRet * Math.sqrt(barsPerYear);

  const rfPerBar = RISK_FREE_RATE / barsPerYear;
  const sharpe = sdRet > 0
    ? ((meanRet - rfPerBar) / sdRet) * Math.sqrt(barsPerYear)
    : null;

  // Max drawdown (use provided drawdown column if present, else recompute)
  let maxDrawdown = 0;
  let peak = rows[0].nav;
  for (const r of rows) {
    peak = Math.max(peak, r.nav);
    const dd = peak > 0 ? (peak - r.nav) / peak : 0;
    maxDrawdown = Math.max(maxDrawdown, dd);
  }

  return {
    totalReturn, cagr, annVol, sharpe, maxDrawdown,
    finalNav, initialNav, numBars: rows.length, barsPerYear,
    rets,
  };
}

/**
 * Render the summary metric cards into #summaryGrid.
 * Uses the *first* NAV series as the primary metrics source, and shows
 * trade count from state.tradesRaw.
 */
function renderSummary() {
  const grid = document.getElementById("summaryGrid");
  grid.innerHTML = "";

  if (state.navSeries.length === 0) {
    grid.innerHTML = `<div class="summary-card full-span">
      <div class="label">No NAV data loaded</div>
      <div class="value" style="font-size:13px;font-weight:500;color:var(--text-muted)">
        Load a nav.csv file to see metrics
      </div>
    </div>`;
    return;
  }

  const primary = state.navSeries[0];
  const m = computeMetrics(primary.rows);

  const cards = [
    { label: "Total Return", value: fmtPct(m.totalReturn), cls: signCls(m.totalReturn) },
    { label: "CAGR", value: fmtPct(m.cagr), cls: signCls(m.cagr) },
    { label: "Annualized Vol", value: fmtPct(m.annVol), cls: "" },
    { label: "Sharpe Ratio", value: fmtNumber(m.sharpe, 3), cls: signCls(m.sharpe) },
    { label: "Max Drawdown", value: fmtPct(m.maxDrawdown), cls: "negative" },
    { label: "Total Trades", value: String(state.tradesRaw.length), cls: "" },
    { label: "Final NAV", value: fmtNumber(m.finalNav, 0), cls: "" },
    { label: "Bars / Year (inferred)", value: String(m.barsPerYear), cls: "" },
  ];

  if (state.navSeries.length > 1) {
    cards.unshift({
      label: "Primary Series",
      value: primary.name,
      cls: "",
    });
  }

  grid.innerHTML = cards.map((c) => `
    <div class="summary-card${c.label === "Primary Series" ? " full-span" : ""}">
      <div class="label">${escapeHtml(c.label)}</div>
      <div class="value ${c.cls}" style="${c.label === "Primary Series" ? "font-size:13px;" : ""}">
        ${escapeHtml(c.value)}
      </div>
    </div>
  `).join("");
}

function signCls(v) {
  if (v === null || v === undefined || Number.isNaN(v)) return "";
  return v >= 0 ? "positive" : "negative";
}

function escapeHtml(s) {
  return String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

/* ===================== 6. CHART RENDERING ================================= */

const PLOTLY_CONFIG = {
  responsive: true,
  displaylogo: false,
  modeBarButtonsToRemove: ["lasso2d", "select2d"],
};

const PLOTLY_LAYOUT_BASE = {
  font: { family: "-apple-system, Segoe UI, Roboto, sans-serif", size: 12, color: "#1f2430" },
  paper_bgcolor: "#ffffff",
  plot_bgcolor: "#ffffff",
  margin: { l: 56, r: 24, t: 24, b: 40 },
  hovermode: "closest",
  legend: { orientation: "h", y: -0.18 },
  xaxis: { gridcolor: "#eef0f3", zeroline: false },
  yaxis: { gridcolor: "#eef0f3", zeroline: false },
};

/**
 * Render the main NAV + drawdown chart with trade entry/exit markers.
 * Supports multiple overlaid NAV series.
 */
function renderNavChart() {
  const el = document.getElementById("navChart");
  const showDrawdown = document.getElementById("toggleDrawdown").checked;
  const showTrades = document.getElementById("toggleTrades").checked;

  if (state.navSeries.length === 0) {
    Plotly.purge(el);
    el.innerHTML = '<div style="display:flex;align-items:center;justify-content:center;height:100%;color:var(--text-muted)">No NAV data loaded</div>';
    return;
  }
  el.innerHTML = "";

  const traces = [];

  // NAV line(s) — primary y-axis
  state.navSeries.forEach((series, idx) => {
    const color = SERIES_COLORS[idx % SERIES_COLORS.length];
    traces.push({
      type: "scatter",
      mode: "lines",
      name: series.name,
      x: series.rows.map((r) => r.ts),
      y: series.rows.map((r) => r.nav),
      line: { color, width: 1.8 },
      yaxis: "y",
      hovertemplate: "%{x|%Y-%m-%d %H:%M}<br>NAV: %{y:,.2f}<extra>" + escapeHtml(series.name) + "</extra>",
    });
  });

  // Drawdown area — secondary y-axis, only for primary series
  if (showDrawdown && state.navSeries.length > 0) {
    const primary = state.navSeries[0];
    traces.push({
      type: "scatter",
      mode: "lines",
      name: "Drawdown (" + primary.name + ")",
      x: primary.rows.map((r) => r.ts),
      y: primary.rows.map((r) => -(r.drawdown ?? 0) * 100),
      line: { color: "#b91c1c", width: 1 },
      fill: "tozeroy",
      fillcolor: "rgba(185, 28, 28, 0.10)",
      yaxis: "y2",
      hovertemplate: "%{x|%Y-%m-%d %H:%M}<br>Drawdown: %{customdata}<extra></extra>",
      customdata: primary.rows.map((r) => fmtPct(r.drawdown ?? 0)),
    });
  }

  // Trade markers — entry (green) and exit (red), placed on primary NAV series
  if (showTrades && state.tradesRaw.length > 0 && state.navSeries.length > 0) {
    const primary = state.navSeries[0];
    const navAt = makeNavLookup(primary.rows);

    const entryX = [], entryY = [], entryText = [];
    const exitX = [], exitY = [], exitText = [];

    state.tradesRaw.forEach((t, idx) => {
      if (t.entry_ts) {
        entryX.push(t.entry_ts);
        entryY.push(navAt(t.entry_ts));
        entryText.push(tradeHoverText(t, "Entry", idx));
      }
      if (t.exit_ts) {
        exitX.push(t.exit_ts);
        exitY.push(navAt(t.exit_ts));
        exitText.push(tradeHoverText(t, "Exit", idx));
      }
    });

    traces.push({
      type: "scatter",
      mode: "markers",
      name: "Trade entry (long/buy)",
      x: entryX, y: entryY,
      marker: { color: "#15803d", size: 8, symbol: "triangle-up", line: { width: 1, color: "#0a4d20" } },
      text: entryText,
      hovertemplate: "%{text}<extra></extra>",
      yaxis: "y",
    });

    traces.push({
      type: "scatter",
      mode: "markers",
      name: "Trade exit (close/sell)",
      x: exitX, y: exitY,
      marker: { color: "#b91c1c", size: 8, symbol: "triangle-down", line: { width: 1, color: "#6f0f0f" } },
      text: exitText,
      hovertemplate: "%{text}<extra></extra>",
      yaxis: "y",
    });
  }

  const layout = Object.assign({}, PLOTLY_LAYOUT_BASE, {
    xaxis: Object.assign({}, PLOTLY_LAYOUT_BASE.xaxis, {
      title: "", type: "date", rangeslider: { visible: false },
    }),
    yaxis: Object.assign({}, PLOTLY_LAYOUT_BASE.yaxis, {
      title: "NAV", side: "left",
    }),
  });

  if (showDrawdown) {
    layout.yaxis2 = {
      title: "Drawdown (%)",
      overlaying: "y",
      side: "right",
      showgrid: false,
      zeroline: false,
      range: [-100, 5],
    };
  }

  Plotly.newPlot(el, traces, layout, PLOTLY_CONFIG);

  // Click on a marker -> select corresponding trade in table
  el.removeAllListeners?.("plotly_click");
  el.on("plotly_click", (ev) => {
    const pt = ev.points && ev.points[0];
    if (!pt) return;
    const traceName = pt.data.name;
    if (traceName !== "Trade entry (long/buy)" && traceName !== "Trade exit (close/sell)") return;
    // Find matching trade by timestamp
    const clickedTs = pt.x;
    const isEntry = traceName.startsWith("Trade entry");
    const idx = state.tradesRaw.findIndex((t) => {
      const ts = isEntry ? t.entry_ts : t.exit_ts;
      return ts && Math.abs(ts.getTime() - new Date(clickedTs).getTime()) < 1000;
    });
    if (idx >= 0) selectTradeByGlobalIndex(idx);
  });
}

/** Build a lookup function: given a Date, returns the nearest NAV value. */
function makeNavLookup(rows) {
  const times = rows.map((r) => r.ts.getTime());
  return function (date) {
    const t = date.getTime();
    // binary search for nearest
    let lo = 0, hi = times.length - 1;
    if (t <= times[0]) return rows[0].nav;
    if (t >= times[hi]) return rows[hi].nav;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (times[mid] < t) lo = mid + 1; else hi = mid;
    }
    const i = lo;
    const prevI = Math.max(0, i - 1);
    // pick nearer of the two
    return (Math.abs(times[i] - t) < Math.abs(times[prevI] - t)) ? rows[i].nav : rows[prevI].nav;
  };
}

function tradeHoverText(t, label, idx) {
  const px = label === "Entry" ? t.entry_price : t.exit_price;
  const ts = label === "Entry" ? t.entry_ts : t.exit_ts;
  return [
    `<b>${escapeHtml(t.instrument)}</b> ${escapeHtml(t.side)} — ${label}`,
    `${fmtDate(ts)}`,
    `Price: ${fmtNumber(px)}`,
    `Qty: ${fmtNumber(t.qty, 0)}`,
    `PnL: ${fmtNumber(t.pnl)}`,
    `Strategy: ${escapeHtml(t.strategy_id)}`,
  ].join("<br>");
}

/**
 * Render the returns/PnL distribution histogram.
 * Mode "hist" -> trade PnL histogram
 * Mode "returns" -> daily returns histogram (from primary NAV series)
 */
function renderReturnsChart() {
  const el = document.getElementById("returnsChart");
  const mode = document.getElementById("returnsMode").value;

  let x, title, color;
  if (mode === "hist") {
    x = state.tradesRaw.map((t) => t.pnl);
    title = "Trade PnL ($)";
    color = "#2563eb";
    if (x.length === 0) {
      Plotly.purge(el);
      el.innerHTML = '<div style="display:flex;align-items:center;justify-content:center;height:100%;color:var(--text-muted)">No trades loaded</div>';
      return;
    }
  } else {
    if (state.navSeries.length === 0) {
      Plotly.purge(el);
      el.innerHTML = '<div style="display:flex;align-items:center;justify-content:center;height:100%;color:var(--text-muted)">No NAV data loaded</div>';
      return;
    }
    const m = computeMetrics(state.navSeries[0].rows);
    x = (m.rets || []).map((r) => r * 100);
    title = "Daily Return (%)";
    color = "#0891b2";
    if (x.length === 0) {
      Plotly.purge(el);
      el.innerHTML = '<div style="display:flex;align-items:center;justify-content:center;height:100%;color:var(--text-muted)">Not enough data</div>';
      return;
    }
  }

  el.innerHTML = "";
  const trace = {
    type: "histogram",
    x,
    marker: { color, opacity: 0.85 },
    nbinsx: 30,
  };

  const layout = Object.assign({}, PLOTLY_LAYOUT_BASE, {
    xaxis: Object.assign({}, PLOTLY_LAYOUT_BASE.xaxis, { title }),
    yaxis: Object.assign({}, PLOTLY_LAYOUT_BASE.yaxis, { title: "Count" }),
    showlegend: false,
    bargap: 0.05,
  });

  Plotly.newPlot(el, [trace], layout, PLOTLY_CONFIG);
}

/**
 * Render cumulative returns chart (linear or log) for all NAV series.
 */
function renderCumChart() {
  const el = document.getElementById("cumChart");
  const mode = document.getElementById("cumMode").value;

  if (state.navSeries.length === 0) {
    Plotly.purge(el);
    el.innerHTML = '<div style="display:flex;align-items:center;justify-content:center;height:100%;color:var(--text-muted)">No NAV data loaded</div>';
    return;
  }
  el.innerHTML = "";

  const traces = state.navSeries.map((series, idx) => {
    const color = SERIES_COLORS[idx % SERIES_COLORS.length];
    const initial = series.rows[0].nav;
    const x = series.rows.map((r) => r.ts);
    let y;
    if (mode === "log") {
      y = series.rows.map((r) => Math.log(r.nav / initial));
    } else {
      y = series.rows.map((r) => (r.nav / initial - 1) * 100);
    }
    return {
      type: "scatter",
      mode: "lines",
      name: series.name,
      x, y,
      line: { color, width: 1.8 },
      hovertemplate: "%{x|%Y-%m-%d %H:%M}<br>" +
        (mode === "log" ? "Log return: %{y:.4f}" : "Cum. return: %{y:.2f}%") +
        "<extra>" + escapeHtml(series.name) + "</extra>",
    };
  });

  const layout = Object.assign({}, PLOTLY_LAYOUT_BASE, {
    xaxis: Object.assign({}, PLOTLY_LAYOUT_BASE.xaxis, { type: "date" }),
    yaxis: Object.assign({}, PLOTLY_LAYOUT_BASE.yaxis, {
      title: mode === "log" ? "Log Return" : "Cumulative Return (%)",
    }),
  });

  Plotly.newPlot(el, traces, layout, PLOTLY_CONFIG);
}

function renderAllCharts() {
  renderNavChart();
  renderReturnsChart();
  renderCumChart();
}

/* ===================== 7. TRADES TABLE ==================================== */

/**
 * Populate the instrument/strategy filter dropdowns from the loaded trades.
 */
function populateFilterOptions() {
  const instSel = document.getElementById("filterInstrument");
  const stratSel = document.getElementById("filterStrategy");

  const instruments = Array.from(new Set(state.tradesRaw.map((t) => t.instrument))).sort();
  const strategies = Array.from(new Set(state.tradesRaw.map((t) => t.strategy_id))).sort();

  const buildOptions = (sel, values, allLabel) => {
    const current = sel.value;
    sel.innerHTML = `<option value="">${allLabel}</option>` +
      values.map((v) => `<option value="${escapeHtml(v)}">${escapeHtml(v)}</option>`).join("");
    if (values.includes(current)) sel.value = current;
  };

  buildOptions(instSel, instruments, "All instruments");
  buildOptions(stratSel, strategies, "All strategies");
}

/**
 * Apply current search/filter/sort settings to state.tradesRaw,
 * producing state.filtered. Resets to page 1.
 */
function applyFiltersAndSort() {
  const search = document.getElementById("searchInput").value.trim().toLowerCase();
  const instrument = document.getElementById("filterInstrument").value;
  const strategy = document.getElementById("filterStrategy").value;
  const side = document.getElementById("filterSide").value;
  const dateFrom = document.getElementById("filterDateFrom").value;
  const dateTo = document.getElementById("filterDateTo").value;

  let rows = state.tradesRaw.map((t, globalIdx) => ({ ...t, _globalIdx: globalIdx }));

  if (instrument) rows = rows.filter((t) => t.instrument === instrument);
  if (strategy) rows = rows.filter((t) => t.strategy_id === strategy);
  if (side) rows = rows.filter((t) => t.side === side);

  if (dateFrom) {
    const from = new Date(dateFrom + "T00:00:00");
    rows = rows.filter((t) => (t.exit_ts || t.entry_ts) && (t.exit_ts || t.entry_ts) >= from);
  }
  if (dateTo) {
    const to = new Date(dateTo + "T23:59:59");
    rows = rows.filter((t) => (t.exit_ts || t.entry_ts) && (t.exit_ts || t.entry_ts) <= to);
  }

  if (search) {
    rows = rows.filter((t) => {
      const haystack = [
        t.instrument, t.strategy_id, t.side, t.qty, t.entry_price, t.exit_price,
        fmtDate(t.entry_ts), fmtDate(t.exit_ts), t.pnl, t.commission, t.slippage,
      ].join(" ").toLowerCase();
      return haystack.includes(search);
    });
  }

  // Sort
  const key = state.sortKey;
  const dir = state.sortDir === "asc" ? 1 : -1;
  rows.sort((a, b) => {
    let va = a[key], vb = b[key];
    if (va instanceof Date) va = va.getTime();
    if (vb instanceof Date) vb = vb.getTime();
    if (va === null || va === undefined) va = -Infinity;
    if (vb === null || vb === undefined) vb = -Infinity;
    if (typeof va === "string") {
      return va.localeCompare(vb) * dir;
    }
    return (va - vb) * dir;
  });

  state.filtered = rows;
  state.page = 1;
  renderTradesTable();
}

/**
 * Render the trades table body, pagination, and table info text based on
 * state.filtered / state.page / state.pageSize.
 */
function renderTradesTable() {
  const tbody = document.getElementById("tradesTableBody");
  const total = state.filtered.length;
  const pageSize = state.pageSize;
  const totalPages = Math.max(1, Math.ceil(total / pageSize));
  if (state.page > totalPages) state.page = totalPages;
  const start = (state.page - 1) * pageSize;
  const pageRows = state.filtered.slice(start, start + pageSize);

  if (pageRows.length === 0) {
    tbody.innerHTML = `<tr class="row-empty"><td colspan="11">No trades match the current filters.</td></tr>`;
  } else {
    tbody.innerHTML = pageRows.map((t) => {
      const sideClass = t.side === "LONG" ? "side-long" : (t.side === "SHORT" ? "side-short" : "");
      const pnlClass = t.pnl >= 0 ? "pnl-pos" : "pnl-neg";
      const selected = t._globalIdx === state.selectedTradeIdx ? " row-selected" : "";
      return `<tr class="trade-row${selected}" data-global-idx="${t._globalIdx}">
        <td>${escapeHtml(t.instrument)}</td>
        <td>${escapeHtml(t.strategy_id)}</td>
        <td><span class="${sideClass}">${escapeHtml(t.side)}</span></td>
        <td>${fmtNumber(t.qty, 0)}</td>
        <td>${fmtNumber(t.entry_price)}</td>
        <td>${fmtNumber(t.exit_price)}</td>
        <td>${fmtDate(t.entry_ts)}</td>
        <td>${fmtDate(t.exit_ts)}</td>
        <td class="${pnlClass}">${fmtNumber(t.pnl)}</td>
        <td>${fmtNumber(t.commission)}</td>
        <td>${fmtNumber(t.slippage)}</td>
      </tr>`;
    }).join("");

    // Row click -> select trade, scroll NAV chart marker into focus (via highlight)
    tbody.querySelectorAll("tr.trade-row").forEach((tr) => {
      tr.addEventListener("click", () => {
        const idx = Number(tr.getAttribute("data-global-idx"));
        selectTradeByGlobalIndex(idx);
      });
    });
  }

  // Table info
  const infoEl = document.getElementById("tableInfo");
  if (total === 0) {
    infoEl.textContent = "Showing 0 of 0 trades";
  } else {
    infoEl.textContent = `Showing ${start + 1}-${Math.min(start + pageSize, total)} of ${total} trades`;
  }

  // Pagination controls
  document.getElementById("pageIndicator").textContent = `Page ${state.page} / ${totalPages}`;
  document.getElementById("prevPage").disabled = state.page <= 1;
  document.getElementById("nextPage").disabled = state.page >= totalPages;

  // Update sort indicator classes on headers
  document.querySelectorAll("#tradesTable thead th").forEach((th) => {
    th.classList.remove("sorted-asc", "sorted-desc");
    if (th.dataset.key === state.sortKey) {
      th.classList.add(state.sortDir === "asc" ? "sorted-asc" : "sorted-desc");
    }
  });
}

/**
 * Select a trade by its index into state.tradesRaw — highlights the row
 * in the table (scrolling it into view) and re-renders the NAV chart with
 * the corresponding markers visually emphasized via a temporary highlight
 * trace.
 */
function selectTradeByGlobalIndex(idx) {
  state.selectedTradeIdx = idx;

  // Ensure the selected trade is visible: jump to its page under current filters
  const posInFiltered = state.filtered.findIndex((t) => t._globalIdx === idx);
  if (posInFiltered >= 0) {
    state.page = Math.floor(posInFiltered / state.pageSize) + 1;
  }
  renderTradesTable();

  // Scroll the selected row into view
  requestAnimationFrame(() => {
    const row = document.querySelector(`tr.trade-row[data-global-idx="${idx}"]`);
    if (row) row.scrollIntoView({ behavior: "smooth", block: "nearest" });
  });

  highlightTradeOnChart(idx);
}

/**
 * Add a temporary highlight trace (larger markers) to the NAV chart for
 * the selected trade's entry/exit points.
 */
function highlightTradeOnChart(idx) {
  const el = document.getElementById("navChart");
  if (!el.data) return; // chart not rendered yet

  const t = state.tradesRaw[idx];
  if (!t || state.navSeries.length === 0) return;

  const navAt = makeNavLookup(state.navSeries[0].rows);

  // Remove any previous highlight traces (identified by a custom flag)
  const keepIdx = [];
  el.data.forEach((tr, i) => { if (!tr._isHighlight) keepIdx.push(i); });
  if (keepIdx.length !== el.data.length) {
    Plotly.deleteTraces(el, el.data.map((_, i) => i).filter((i) => !keepIdx.includes(i)));
  }

  const hx = [], hy = [], htext = [];
  if (t.entry_ts) { hx.push(t.entry_ts); hy.push(navAt(t.entry_ts)); htext.push(tradeHoverText(t, "Entry", idx)); }
  if (t.exit_ts)  { hx.push(t.exit_ts);  hy.push(navAt(t.exit_ts));  htext.push(tradeHoverText(t, "Exit", idx)); }

  Plotly.addTraces(el, [{
    type: "scatter",
    mode: "markers",
    name: "Selected trade",
    x: hx, y: hy,
    marker: { color: "#f59e0b", size: 16, symbol: "circle-open", line: { width: 3, color: "#b45309" } },
    text: htext,
    hovertemplate: "%{text}<extra></extra>",
    showlegend: false,
    _isHighlight: true,
  }]);
}

/**
 * Export the currently filtered trades to a CSV file and trigger a download.
 */
function exportFilteredCsv() {
  const headers = [
    "instrument", "strategy_id", "side", "qty", "entry_price", "exit_price",
    "entry_ts", "exit_ts", "pnl", "commission", "slippage",
  ];
  const lines = [headers.join(",")];
  for (const t of state.filtered) {
    lines.push([
      csvEscape(t.instrument),
      csvEscape(t.strategy_id),
      csvEscape(t.side),
      t.qty,
      t.entry_price,
      t.exit_price,
      t.entry_ts ? t.entry_ts.toISOString() : "",
      t.exit_ts ? t.exit_ts.toISOString() : "",
      t.pnl,
      t.commission,
      t.slippage,
    ].join(","));
  }
  downloadTextFile(lines.join("\n"), "trades_filtered.csv", "text/csv");
}

function downloadTextFile(content, filename, mime) {
  const blob = new Blob([content], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

/* ===================== 8. FILE LOADING / DROPZONES ========================= */

function renderNavFileList() {
  const ul = document.getElementById("navFileList");
  if (state.navSeries.length === 0) {
    ul.innerHTML = state.isDemo
      ? `<li><span><span class="file-color-dot" style="background:${SERIES_COLORS[0]}"></span>demo_nav.csv (embedded)</span></li>`
      : "";
    return;
  }
  ul.innerHTML = state.navSeries.map((s, i) => `
    <li>
      <span><span class="file-color-dot" style="background:${SERIES_COLORS[i % SERIES_COLORS.length]}"></span>${escapeHtml(s.name)} (${s.rows.length} rows)</span>
      <button class="btn btn-small btn-secondary" data-remove-nav="${i}" style="padding:2px 6px;">&times;</button>
    </li>
  `).join("");

  ul.querySelectorAll("[data-remove-nav]").forEach((btn) => {
    btn.addEventListener("click", (e) => {
      e.stopPropagation();
      const i = Number(btn.getAttribute("data-remove-nav"));
      state.navSeries.splice(i, 1);
      state.isDemo = state.navSeries.length === 0 && state.tradesRaw.length === 0;
      updateModeIndicator();
      renderNavFileList();
      renderSummary();
      renderAllCharts();
    });
  });
}

function renderTradesFileList() {
  const ul = document.getElementById("tradesFileList");
  if (state.tradesRaw.length === 0) {
    ul.innerHTML = "";
    return;
  }
  ul.innerHTML = `<li>
    <span>${escapeHtml(state.tradesFileName || "trades.csv")} (${state.tradesRaw.length} trades)</span>
    <button class="btn btn-small btn-secondary" id="removeTradesBtn" style="padding:2px 6px;">&times;</button>
  </li>`;
  document.getElementById("removeTradesBtn")?.addEventListener("click", () => {
    state.tradesRaw = [];
    state.filtered = [];
    state.tradesFileName = null;
    state.isDemo = state.navSeries.length === 0 && state.tradesRaw.length === 0;
    updateModeIndicator();
    renderTradesFileList();
    populateFilterOptions();
    applyFiltersAndSort();
    renderSummary();
    renderAllCharts();
  });
}

function updateModeIndicator() {
  const el = document.getElementById("modeIndicator");
  if (state.isDemo) {
    el.textContent = "DEMO MODE";
    el.className = "mode-badge mode-demo";
  } else {
    el.textContent = "LOADED DATA";
    el.className = "mode-badge mode-live";
  }
}

/**
 * Handle a list of File objects dropped/selected for NAV CSVs.
 * Each file becomes a separate overlaid series.
 */
async function handleNavFiles(fileList) {
  const files = Array.from(fileList);
  for (const file of files) {
    try {
      const text = await readFileAsText(file);
      const series = parseNavCsv(text, file.name);
      if (series.rows.length === 0) {
        alert(`"${file.name}": no valid rows found (check timestamp/nav columns).`);
        continue;
      }
      state.navSeries.push(series);
    } catch (err) {
      alert(err.message || String(err));
    }
  }
  state.isDemo = false;
  updateModeIndicator();
  renderNavFileList();
  renderSummary();
  renderAllCharts();
}

/**
 * Handle a single Trades CSV file.
 */
async function handleTradesFile(file) {
  try {
    const text = await readFileAsText(file);
    const trades = parseTradesCsv(text);
    if (trades.length === 0) {
      alert(`"${file.name}": no valid trade rows found.`);
      return;
    }
    state.tradesRaw = trades;
    state.tradesFileName = file.name;
    state.selectedTradeIdx = null;
  } catch (err) {
    alert(err.message || String(err));
    return;
  }
  state.isDemo = false;
  updateModeIndicator();
  renderTradesFileList();
  populateFilterOptions();
  applyFiltersAndSort();
  renderSummary();
  renderAllCharts();
}

/* ===================== 9. INITIALIZATION & WIRING ========================== */

function loadDemoData() {
  state.navSeries = [parseNavCsv(DEMO_NAV_CSV, "demo_nav.csv")];
  state.tradesRaw = parseTradesCsv(DEMO_TRADES_CSV);
  state.tradesFileName = "demo_trades.csv";
  state.selectedTradeIdx = null;
  state.isDemo = true;
  updateModeIndicator();
  renderNavFileList();
  renderTradesFileList();
  populateFilterOptions();
  applyFiltersAndSort();
  renderSummary();
  renderAllCharts();
}

function setupDropzone(zoneEl, inputEl, onFiles) {
  zoneEl.addEventListener("click", () => inputEl.click());
  zoneEl.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") { e.preventDefault(); inputEl.click(); }
  });
  inputEl.addEventListener("change", (e) => {
    if (e.target.files.length > 0) onFiles(e.target.files);
    inputEl.value = "";
  });
  ["dragenter", "dragover"].forEach((evt) => {
    zoneEl.addEventListener(evt, (e) => {
      e.preventDefault(); e.stopPropagation();
      zoneEl.classList.add("dragover");
    });
  });
  ["dragleave", "drop"].forEach((evt) => {
    zoneEl.addEventListener(evt, (e) => {
      e.preventDefault(); e.stopPropagation();
      zoneEl.classList.remove("dragover");
    });
  });
  zoneEl.addEventListener("drop", (e) => {
    const files = e.dataTransfer.files;
    if (files.length > 0) onFiles(files);
  });
}

function initEventListeners() {
  // Dropzones
  setupDropzone(
    document.getElementById("navDropzone"),
    document.getElementById("navFileInput"),
    (files) => handleNavFiles(files)
  );
  setupDropzone(
    document.getElementById("tradesDropzone"),
    document.getElementById("tradesFileInput"),
    (files) => handleTradesFile(files[0])
  );

  // Reset to demo
  document.getElementById("resetBtn").addEventListener("click", () => {
    state.navSeries = [];
    state.tradesRaw = [];
    state.filtered = [];
    state.selectedTradeIdx = null;
    loadDemoData();
  });

  // Chart toggles
  document.getElementById("toggleDrawdown").addEventListener("change", renderNavChart);
  document.getElementById("toggleTrades").addEventListener("change", renderNavChart);
  document.getElementById("returnsMode").addEventListener("change", renderReturnsChart);
  document.getElementById("cumMode").addEventListener("change", renderCumChart);

  // PNG export buttons
  document.getElementById("exportNavPng").addEventListener("click", () => {
    Plotly.downloadImage(document.getElementById("navChart"), { format: "png", filename: "nav_chart", height: 600, width: 1200 });
  });
  document.getElementById("exportRetPng").addEventListener("click", () => {
    Plotly.downloadImage(document.getElementById("returnsChart"), { format: "png", filename: "returns_chart", height: 500, width: 900 });
  });
  document.getElementById("exportCumPng").addEventListener("click", () => {
    Plotly.downloadImage(document.getElementById("cumChart"), { format: "png", filename: "cumulative_returns", height: 500, width: 900 });
  });

  // Table sorting
  document.querySelectorAll("#tradesTable thead th").forEach((th) => {
    th.addEventListener("click", () => {
      const key = th.dataset.key;
      if (state.sortKey === key) {
        state.sortDir = state.sortDir === "asc" ? "desc" : "asc";
      } else {
        state.sortKey = key;
        state.sortDir = "asc";
      }
      applyFiltersAndSort();
    });
  });

  // Filters
  document.getElementById("searchInput").addEventListener("input", debounce(applyFiltersAndSort, 200));
  document.getElementById("filterInstrument").addEventListener("change", applyFiltersAndSort);
  document.getElementById("filterStrategy").addEventListener("change", applyFiltersAndSort);
  document.getElementById("filterSide").addEventListener("change", applyFiltersAndSort);
  document.getElementById("filterDateFrom").addEventListener("change", applyFiltersAndSort);
  document.getElementById("filterDateTo").addEventListener("change", applyFiltersAndSort);

  document.getElementById("clearFilters").addEventListener("click", () => {
    document.getElementById("searchInput").value = "";
    document.getElementById("filterInstrument").value = "";
    document.getElementById("filterStrategy").value = "";
    document.getElementById("filterSide").value = "";
    document.getElementById("filterDateFrom").value = "";
    document.getElementById("filterDateTo").value = "";
    applyFiltersAndSort();
  });

  // Pagination
  document.getElementById("prevPage").addEventListener("click", () => {
    if (state.page > 1) { state.page--; renderTradesTable(); }
  });
  document.getElementById("nextPage").addEventListener("click", () => {
    const totalPages = Math.max(1, Math.ceil(state.filtered.length / state.pageSize));
    if (state.page < totalPages) { state.page++; renderTradesTable(); }
  });
  document.getElementById("pageSize").addEventListener("change", (e) => {
    state.pageSize = Number(e.target.value);
    state.page = 1;
    renderTradesTable();
  });

  // CSV export
  document.getElementById("exportCsv").addEventListener("click", exportFilteredCsv);

  // Re-render on resize for responsive charts (Plotly responsive:true mostly
  // handles this, but force a resize after layout shifts e.g. sidebar collapse)
  window.addEventListener("resize", debounce(() => {
    ["navChart", "returnsChart", "cumChart"].forEach((id) => {
      const el = document.getElementById(id);
      if (el && el.data) Plotly.Plots.resize(el);
    });
  }, 150));
}

/** Simple debounce utility. */
function debounce(fn, ms) {
  let t;
  return (...args) => {
    clearTimeout(t);
    t = setTimeout(() => fn(...args), ms);
  };
}

// ---- Bootstrap ----
document.addEventListener("DOMContentLoaded", () => {
  initEventListeners();
  loadDemoData();
});
