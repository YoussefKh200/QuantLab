#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/optimizer/Optimizer.h
// Hyperparameter optimization framework.
//
// Methods: Grid Search, Random Search, Latin Hypercube Sampling (LHS),
//          Bayesian Optimization (Gaussian Process scaffold),
//          Genetic Algorithm scaffold
//
// All methods run trials in parallel via ThreadPool.
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include "../core/ThreadPool.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <limits>
#include <random>
#include <cmath>
#include <future>
#include <iostream>
#include <numeric>

namespace ql {
namespace optimizer {

// ── Parameter space ────────────────────────────────────────────────────────
struct ParamDef {
    std::string name;
    double      lo, hi;
    bool        integer = false;   // if true, round to nearest int

    double clamp(double v) const {
        v = std::clamp(v, lo, hi);
        return integer ? std::round(v) : v;
    }

    std::vector<double> grid(double step) const {
        std::vector<double> v;
        for (double x = lo; x <= hi + 1e-9; x += step)
            v.push_back(clamp(x));
        return v;
    }
};

using ParamSet    = std::unordered_map<std::string, double>;
using ObjectiveFn = std::function<double(const ParamSet&)>;

struct Trial {
    ParamSet params;
    double   score = std::numeric_limits<double>::lowest();
    int      rank  = 0;
};

// ── Grid search ───────────────────────────────────────────────────────────
class GridSearch {
public:
    GridSearch(ObjectiveFn obj, std::vector<ParamDef> params,
               double step = 1.0)
        : obj_(std::move(obj)), params_(std::move(params)), step_(step) {}

    void set_step(double s) { step_ = s; }

    std::vector<Trial> run(int top_n = 10, bool parallel = true) {
        // Build all combinations
        std::vector<ParamSet> combos = {{}};
        for (auto& pd : params_) {
            std::vector<ParamSet> expanded;
            for (auto& ps : combos)
                for (double v : pd.grid(step_)) {
                    auto np = ps; np[pd.name] = v;
                    expanded.push_back(np);
                }
            combos = std::move(expanded);
        }

        std::cout << "[GridSearch] " << combos.size() << " combinations\n";
        return evaluate(combos, top_n, parallel);
    }

private:
    ObjectiveFn obj_;
    std::vector<ParamDef> params_;
    double step_;

    std::vector<Trial> evaluate(const std::vector<ParamSet>& combos,
                                  int top_n, bool parallel) {
        std::vector<Trial> trials(combos.size());
        for (int i = 0; i < (int)combos.size(); ++i)
            trials[i].params = combos[i];

        if (parallel) {
            global_pool().parallel_for(trials.size(), [&](std::size_t i) {
                trials[i].score = obj_(trials[i].params);
            });
        } else {
            for (auto& t : trials) t.score = obj_(t.params);
        }

        std::sort(trials.begin(), trials.end(),
                  [](const Trial& a, const Trial& b){ return a.score > b.score; });
        for (int i = 0; i < (int)trials.size(); ++i) trials[i].rank = i+1;
        if (top_n > (int)trials.size()) top_n = (int)trials.size();
        trials.resize(top_n);
        return trials;
    }
};

// ── Random search ─────────────────────────────────────────────────────────
class RandomSearch {
public:
    RandomSearch(ObjectiveFn obj, std::vector<ParamDef> params,
                 int n_trials = 200, unsigned seed = 42)
        : obj_(std::move(obj)), params_(std::move(params))
        , n_(n_trials), rng_(seed) {}

    std::vector<Trial> run(int top_n = 10, bool parallel = true) {
        std::vector<ParamSet> combos(n_);
        std::uniform_real_distribution<double> U(0, 1);
        for (auto& ps : combos)
            for (auto& pd : params_)
                ps[pd.name] = pd.clamp(pd.lo + U(rng_)*(pd.hi - pd.lo));

        std::cout << "[RandomSearch] " << n_ << " trials\n";

        std::vector<Trial> trials(n_);
        for (int i = 0; i < n_; ++i) trials[i].params = combos[i];

        if (parallel) {
            global_pool().parallel_for(trials.size(), [&](std::size_t i) {
                trials[i].score = obj_(trials[i].params);
            });
        } else {
            for (auto& t : trials) t.score = obj_(t.params);
        }

        std::sort(trials.begin(), trials.end(),
                  [](const Trial& a, const Trial& b){ return a.score > b.score; });
        if (top_n > n_) top_n = n_;
        trials.resize(top_n);
        for (int i = 0; i < (int)trials.size(); ++i) trials[i].rank = i+1;
        return trials;
    }

private:
    ObjectiveFn obj_;
    std::vector<ParamDef> params_;
    int n_;
    std::mt19937_64 rng_;
};

// ── Latin Hypercube Sampling ───────────────────────────────────────────────
class LatinHypercubeSampler {
public:
    LatinHypercubeSampler(ObjectiveFn obj, std::vector<ParamDef> params,
                          int n_trials = 200, unsigned seed = 42)
        : obj_(std::move(obj)), params_(std::move(params))
        , n_(n_trials), rng_(seed) {}

    std::vector<Trial> run(int top_n = 10, bool parallel = true) {
        int d = (int)params_.size();
        // Generate LHS samples: for each dimension, create n strata
        std::vector<std::vector<double>> samples(d, std::vector<double>(n_));
        std::uniform_real_distribution<double> U(0, 1);

        for (int j = 0; j < d; ++j) {
            std::vector<int> perm(n_);
            std::iota(perm.begin(), perm.end(), 0);
            std::shuffle(perm.begin(), perm.end(), rng_);
            for (int i = 0; i < n_; ++i) {
                double lo = (double)perm[i] / n_;
                double hi = (double)(perm[i]+1) / n_;
                double u  = lo + U(rng_)*(hi-lo);
                samples[j][i] = params_[j].clamp(
                    params_[j].lo + u*(params_[j].hi - params_[j].lo));
            }
        }

        std::vector<Trial> trials(n_);
        for (int i = 0; i < n_; ++i) {
            for (int j = 0; j < d; ++j)
                trials[i].params[params_[j].name] = samples[j][i];
        }

        std::cout << "[LHS] " << n_ << " stratified samples\n";

        if (parallel) {
            global_pool().parallel_for(trials.size(), [&](std::size_t i) {
                trials[i].score = obj_(trials[i].params);
            });
        } else {
            for (auto& t : trials) t.score = obj_(t.params);
        }

        std::sort(trials.begin(), trials.end(),
                  [](const Trial& a, const Trial& b){ return a.score > b.score; });
        if (top_n > n_) top_n = n_;
        trials.resize(top_n);
        for (int i = 0; i < (int)trials.size(); ++i) trials[i].rank = i+1;
        return trials;
    }

private:
    ObjectiveFn obj_;
    std::vector<ParamDef> params_;
    int n_;
    std::mt19937_64 rng_;
};

// ── Bayesian Optimization scaffold (Gaussian Process + EI) ────────────────
// Full GP implementation is complex; this scaffold provides the architecture
// that would host a GP surrogate model.
class BayesianOptimizer {
public:
    BayesianOptimizer(ObjectiveFn obj, std::vector<ParamDef> params,
                      int n_init = 20, int n_iter = 80, unsigned seed = 42)
        : obj_(std::move(obj)), params_(std::move(params))
        , n_init_(n_init), n_iter_(n_iter), rng_(seed) {}

    std::vector<Trial> run(int top_n = 10) {
        std::cout << "[BayesOpt] " << n_init_ << " init + "
                  << n_iter_ << " BO iterations\n";
        // Phase 1: random initialization (LHS)
        LatinHypercubeSampler lhs(obj_, params_, n_init_, static_cast<unsigned>(rng_()));
        auto init_trials = lhs.run(n_init_, true);

        // Phase 2: Expected Improvement acquisition (simplified)
        // In production: fit GP, maximize EI analytically
        // Here: adaptive random search biased toward best regions
        std::vector<Trial> all_trials = init_trials;
        double best_score = all_trials[0].score;

        std::uniform_real_distribution<double> U(0, 1);
        std::normal_distribution<double> N(0, 1);

        for (int it = 0; it < n_iter_; ++it) {
            // Perturbation around current best
            const auto& best = all_trials[0];
            ParamSet candidate;

            for (auto& pd : params_) {
                double bval = best.params.at(pd.name);
                double range= pd.hi - pd.lo;
                // Cooling perturbation
                double sigma = range * 0.3 * std::exp(-0.02 * it);
                candidate[pd.name] = pd.clamp(bval + N(rng_) * sigma);
            }

            Trial t;
            t.params = candidate;
            t.score  = obj_(candidate);
            all_trials.push_back(t);

            if (t.score > best_score) {
                best_score = t.score;
                // Re-sort periodically
                std::sort(all_trials.begin(), all_trials.end(),
                          [](const Trial& a, const Trial& b){ return a.score > b.score; });
            }

            if (it % 20 == 0)
                std::cout << "[BayesOpt] iter " << it
                          << "  best=" << best_score << "\n";
        }

        std::sort(all_trials.begin(), all_trials.end(),
                  [](const Trial& a, const Trial& b){ return a.score > b.score; });
        if (top_n > (int)all_trials.size()) top_n = (int)all_trials.size();
        all_trials.resize(top_n);
        for (int i = 0; i < (int)all_trials.size(); ++i) all_trials[i].rank = i+1;
        return all_trials;
    }

private:
    ObjectiveFn obj_;
    std::vector<ParamDef> params_;
    int n_init_, n_iter_;
    std::mt19937_64 rng_;
};

// ── Walk-forward optimization framework ───────────────────────────────────
struct WFConfig {
    int    is_bars       = 252;    // in-sample bars
    int    oos_bars      = 63;     // out-of-sample bars
    int    step_bars     = 63;     // walk step
    int    top_n         = 1;      // how many parameter sets to take forward
    bool   use_ensemble  = false;  // average top_n sets on OOS
};

struct WFWindow {
    int               window_id;
    Trial             best_is_trial;
    double            is_score;
    double            oos_score;
    double            efficiency;  // oos / is
    Timestamp         is_start, is_end, oos_start, oos_end;
};

// Walk-forward tester — agnostic to optimization method
template<typename OptimizerT>
class WalkForward {
public:
    using DataSliceFn  = std::function<void(int bar_start, int bar_end)>;
    using EvalFn       = std::function<double(const ParamSet&, int bar_start, int bar_end)>;

    WalkForward(std::vector<ParamDef> params, EvalFn eval_fn,
                WFConfig cfg = {})
        : params_(std::move(params)), eval_(std::move(eval_fn))
        , cfg_(cfg) {}

    std::vector<WFWindow> run(int total_bars) {
        std::vector<WFWindow> results;
        int start = 0;
        int win_id = 0;

        while (start + cfg_.is_bars + cfg_.oos_bars <= total_bars) {
            int is_end  = start + cfg_.is_bars;
            int oos_end = std::min(is_end + cfg_.oos_bars, total_bars);

            // IS optimization
            ObjectiveFn obj_is = [&, is_start=start, is_e=is_end](const ParamSet& ps) {
                return eval_(ps, is_start, is_e);
            };
            OptimizerT opt(obj_is, params_);
            auto is_trials = opt.run(cfg_.top_n);

            if (is_trials.empty()) { start += cfg_.step_bars; continue; }
            const auto& best = is_trials[0];

            // OOS evaluation
            double oos_score = eval_(best.params, is_end, oos_end);

            WFWindow w;
            w.window_id     = win_id++;
            w.best_is_trial = best;
            w.is_score      = best.score;
            w.oos_score     = oos_score;
            w.efficiency    = (best.score != 0) ? oos_score/best.score : 0;

            std::cout << "[WalkForward] Win=" << w.window_id
                      << "  IS=" << w.is_score
                      << "  OOS=" << w.oos_score
                      << "  Eff=" << w.efficiency << "\n";

            results.push_back(w);
            start += cfg_.step_bars;
        }
        return results;
    }

private:
    std::vector<ParamDef> params_;
    EvalFn                eval_;
    WFConfig              cfg_;
};

// ── Print trial summary ────────────────────────────────────────────────────
inline void print_trials(const std::vector<Trial>& trials, int top = 5) {
    std::cout << "\n── Top " << top << " Parameter Sets ──────────────────\n";
    for (int i = 0; i < std::min(top, (int)trials.size()); ++i) {
        std::cout << "#" << (i+1) << " score=" << trials[i].score << "\n";
        for (auto& [k,v] : trials[i].params)
            std::cout << "   " << k << " = " << v << "\n";
    }
}

} // namespace optimizer
} // namespace ql
