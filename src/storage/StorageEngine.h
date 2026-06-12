#pragma once
#include "../core/Types.h"
#include "../portfolio/Portfolio.h"
#include "../alpha_factory/AlphaFactory.h"
#include "../ranking/StrategyRanker.h"
#include "../ranking/StrategyRegistry.h"
#include "../validation/WalkForwardValidator.h"
#include <string>
#include <fstream>
#include <filesystem>
#include <vector>

#ifdef QL_USE_SQLITE
#include <sqlite3.h>
#endif

namespace ql {
namespace storage {

namespace fs = std::filesystem;

inline std::string csv_escape(const std::string& s) {
    if (s.find(',')==std::string::npos && s.find('"')==std::string::npos
        && s.find('\n')==std::string::npos)
        return s;
    std::string out = "\"";
    for (char c : s) { if (c=='"') out += "\"\""; else out += c; }
    out += "\"";
    return out;
}

class FlatFileStorage {
public:
    explicit FlatFileStorage(fs::path root = "research_db") : root_(root) {
        fs::create_directories(root_);
        fs::create_directories(root_ / "alphas");
        fs::create_directories(root_ / "strategies");
        fs::create_directories(root_ / "trades");
        fs::create_directories(root_ / "nav_curves");
        fs::create_directories(root_ / "walkforward");
        fs::create_directories(root_ / "lifecycle");
    }

    void persist_alphas(const std::vector<alpha::AlphaCandidate>& candidates,
                        const std::string& tag = "latest") {
        auto path = root_ / "alphas" / (tag + ".csv");
        std::ofstream f(path);
        f << "id,name,factor_type,sharpe_is,sortino,calmar,cagr,max_drawdown,"
             "win_rate,profit_factor,mean_ic,ic_std,icir,ic_half_life,"
             "sharpe_mc_p5,sharpe_mc_p50,prob_positive_oos,"
             "turnover_annual,composite_score,rank,promoted\n";
        for (auto& c : candidates) {
            f << c.id << "," << csv_escape(c.name) << "," << c.factor_type << ","
              << c.sharpe_is << "," << c.sortino << "," << c.calmar << ","
              << c.cagr << "," << c.max_drawdown << "," << c.win_rate << ","
              << c.profit_factor << "," << c.mean_ic << "," << c.ic_std << ","
              << c.icir << "," << c.ic_half_life << ","
              << c.sharpe_mc_p5 << "," << c.sharpe_mc_p50 << ","
              << c.prob_positive_oos << "," << c.turnover_annual << ","
              << c.composite_score << "," << c.rank << ","
              << (c.promoted?1:0) << "\n";
        }
    }

    void persist_rankings(const std::vector<ranking::StrategyEntry>& entries,
                          const std::string& tag = "latest") {
        auto path = root_ / "strategies" / (tag + ".csv");
        std::ofstream f(path);
        f << "id,name,sharpe,sortino,calmar,max_dd,cagr,win_rate,profit_factor,"
             "wf_sharpe,wf_robustness,composite_score,regime_stability,"
             "diversification,fitness_score,composite_rank,fitness_rank\n";
        for (auto& e : entries) {
            double wf_sh = e.wf ? e.wf->wf_sharpe : 0;
            double rob   = e.robustness ? e.robustness->overall_robustness_score : 0;
            f << csv_escape(e.id) << "," << csv_escape(e.name) << ","
              << e.perf.sharpe_ratio << "," << e.perf.sortino_ratio << ","
              << e.perf.calmar_ratio << "," << e.perf.max_drawdown << ","
              << e.perf.cagr << "," << e.perf.win_rate << ","
              << e.perf.profit_factor << "," << wf_sh << "," << rob << ","
              << e.composite_score << "," << e.regime_stability_cache << ","
              << e.diversification_benefit << "," << e.fitness_score << ","
              << e.composite_rank << "," << e.fitness_rank << "\n";
        }
    }

    void persist_trades(const std::vector<TradeRecord>& trades,
                        const std::string& strategy_name) {
        auto path = root_ / "trades" / (strategy_name + ".csv");
        std::ofstream f(path);
        f << "instrument,strategy_id,side,qty,entry_price,exit_price,"
             "entry_ts,exit_ts,pnl,commission,slippage\n";
        for (auto& t : trades) {
            f << t.instrument << "," << t.strategy_id << ","
              << (t.side==OrderSide::Buy?"LONG":"SHORT") << ","
              << t.qty << "," << t.entry_price << "," << t.exit_price << ","
              << t.entry_ts << "," << t.exit_ts << ","
              << t.pnl << "," << t.commission << "," << t.slippage << "\n";
        }
    }

    void persist_nav_curve(const std::vector<NAVPoint>& curve,
                           const std::string& strategy_name) {
        auto path = root_ / "nav_curves" / (strategy_name + ".csv");
        std::ofstream f(path);
        f << "timestamp,nav,cash,gross_exposure,net_exposure,leverage,"
             "daily_pnl,cum_pnl,drawdown\n";
        for (auto& p : curve)
            f << p.ts << "," << p.nav << "," << p.cash << ","
              << p.gross_exposure << "," << p.net_exposure << ","
              << p.leverage << "," << p.daily_pnl << "," << p.cum_pnl << ","
              << p.drawdown << "\n";
    }

    void persist_walkforward(const validation::WalkForwardResult& wf,
                             const std::string& strategy_name) {
        auto path = root_ / "walkforward" / (strategy_name + ".csv");
        std::ofstream f(path);
        f << "window_id,is_start,is_end,oos_start,oos_end,is_sharpe,"
             "oos_sharpe,oos_cagr,oos_max_dd,oos_total_ret,efficiency,profitable\n";
        for (auto& w : wf.windows) {
            f << w.window_id << "," << w.is_start_bar << "," << w.is_end_bar << ","
              << w.oos_start_bar << "," << w.oos_end_bar << ","
              << w.is_sharpe << "," << w.oos_sharpe << "," << w.oos_cagr << ","
              << w.oos_max_dd << "," << w.oos_total_ret << "," << w.efficiency << ","
              << (w.is_profitable?1:0) << "\n";
        }
        auto summary_path = root_ / "walkforward" / (strategy_name + "_summary.csv");
        std::ofstream sf(summary_path);
        sf << "metric,value\n";
        sf << "wf_sharpe," << wf.wf_sharpe << "\n";
        sf << "wf_cagr," << wf.wf_cagr << "\n";
        sf << "wf_max_dd," << wf.wf_max_dd << "\n";
        sf << "mean_efficiency," << wf.mean_efficiency << "\n";
        sf << "pct_profitable_windows," << wf.pct_profitable_windows << "\n";
        sf << "overfitting_prob," << wf.overfitting_prob << "\n";
        sf << "robustness_score," << wf.robustness_score << "\n";
    }

    void persist_lifecycle(const ranking::StrategyRegistry& registry) {
        auto path = root_ / "lifecycle" / "events.csv";
        std::ofstream f(path);
        f << "strategy_id,strategy_name,ts,from_state,to_state,reason\n";
        for (auto& [id, s] : registry.all())
            for (auto& ev : s.history)
                f << csv_escape(id) << "," << csv_escape(s.name) << "," << ev.ts << ","
                  << ranking::lifecycle_name(ev.from) << ","
                  << ranking::lifecycle_name(ev.to) << ","
                  << csv_escape(ev.reason) << "\n";

        auto alerts_path = root_ / "lifecycle" / "alerts.csv";
        std::ofstream af(alerts_path);
        af << "ts,strategy_id,type,severity,metric_value,baseline_value,message\n";
        for (auto& a : registry.alert_log()) {
            const char* sev = a.severity==ranking::AlertSeverity::Critical?"CRITICAL"
                             : a.severity==ranking::AlertSeverity::Warning?"WARNING":"INFO";
            af << a.ts << "," << csv_escape(a.strategy_id) << "," << a.type << ","
               << sev << "," << a.metric_value << "," << a.baseline_value << ","
               << csv_escape(a.message) << "\n";
        }
    }

    fs::path root() const { return root_; }

private:
    fs::path root_;
};

#ifdef QL_USE_SQLITE
class SQLiteStorage {
public:
    explicit SQLiteStorage(const std::string& db_path = "research.db") {
        if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK)
            throw std::runtime_error("Cannot open SQLite DB: " + db_path);
        init_schema();
    }
    ~SQLiteStorage() { if (db_) sqlite3_close(db_); }

    void persist_alphas(const std::vector<alpha::AlphaCandidate>& candidates) {
        exec("BEGIN TRANSACTION;");
        const char* sql =
            "INSERT OR REPLACE INTO alphas "
            "(id,name,factor_type,sharpe_is,icir,max_drawdown,composite_score,rank,promoted) "
            "VALUES (?,?,?,?,?,?,?,?,?);";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        for (auto& c : candidates) {
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, (sqlite3_int64)c.id);
            sqlite3_bind_text (stmt, 2, c.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (stmt, 3, c.factor_type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt,4, c.sharpe_is);
            sqlite3_bind_double(stmt,5, c.icir);
            sqlite3_bind_double(stmt,6, c.max_drawdown);
            sqlite3_bind_double(stmt,7, c.composite_score);
            sqlite3_bind_int   (stmt,8, c.rank);
            sqlite3_bind_int   (stmt,9, c.promoted?1:0);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
        exec("COMMIT;");
    }

private:
    void init_schema() {
        exec(
          "CREATE TABLE IF NOT EXISTS alphas ("
          " id INTEGER PRIMARY KEY, name TEXT, factor_type TEXT,"
          " sharpe_is REAL, icir REAL, max_drawdown REAL,"
          " composite_score REAL, rank INTEGER, promoted INTEGER);"
          "CREATE TABLE IF NOT EXISTS strategies ("
          " id TEXT PRIMARY KEY, name TEXT, sharpe REAL, max_dd REAL,"
          " fitness_score REAL, fitness_rank INTEGER);"
          "CREATE TABLE IF NOT EXISTS lifecycle_events ("
          " strategy_id TEXT, ts INTEGER, from_state TEXT, to_state TEXT, reason TEXT);"
        );
    }

    void exec(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error("SQLite error: " + msg);
        }
    }

    sqlite3* db_ = nullptr;
};
#endif

} // namespace storage
} // namespace ql
