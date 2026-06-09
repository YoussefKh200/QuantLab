#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/portfolio/PortfolioConstruction.h
// Institutional portfolio construction algorithms.
//
// Methods: Equal Weight, Inverse Vol, Risk Parity, Min Variance,
//          Max Sharpe, Factor-Weighted, Hierarchical Risk Parity (HRP)
//
// Optimizer uses gradient descent / analytical solutions where available.
// Black-Litterman scaffold included for future extension.
// ═══════════════════════════════════════════════════════════════════════════
#include "../core/Types.h"
#include <vector>
#include <string>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <unordered_map>
#include <stdexcept>
#include <functional>

namespace ql {
namespace construction {

using WeightVec  = std::vector<double>;
using CovMatrix  = std::vector<std::vector<double>>;
using ReturnVec  = std::vector<double>;   // expected returns

// ── Covariance / correlation utils ────────────────────────────────────────
inline CovMatrix compute_cov(
    const std::vector<std::vector<double>>& ret_matrix,  // [asset][bar]
    int bars_per_year = 252)
{
    int n = (int)ret_matrix.size();
    CovMatrix cov(n, std::vector<double>(n, 0));
    for (int i = 0; i < n; ++i) {
        for (int j = i; j < n; ++j) {
            const auto& ri = ret_matrix[i];
            const auto& rj = ret_matrix[j];
            int len = (int)std::min(ri.size(), rj.size());
            if (len < 2) continue;
            double mi = 0, mj = 0;
            for (int k = 0; k < len; ++k) { mi += ri[k]; mj += rj[k]; }
            mi /= len; mj /= len;
            double c = 0;
            for (int k = 0; k < len; ++k) c += (ri[k]-mi)*(rj[k]-mj);
            c = c/(len-1) * bars_per_year;  // annualised
            cov[i][j] = cov[j][i] = c;
        }
    }
    return cov;
}

inline double port_var(const WeightVec& w, const CovMatrix& cov) {
    int n = (int)w.size();
    double v = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            v += w[i]*w[j]*cov[i][j];
    return v;
}

inline double port_ret(const WeightVec& w, const ReturnVec& mu) {
    double r = 0;
    for (int i = 0; i < (int)w.size(); ++i) r += w[i]*mu[i];
    return r;
}

inline WeightVec normalize(WeightVec w) {
    double s = 0;
    for (double x : w) s += std::max(x, 0.0);
    if (s < 1e-12) { std::fill(w.begin(), w.end(), 1.0/w.size()); return w; }
    for (double& x : w) x = std::max(x, 0.0) / s;
    return w;
}

// ── 1. Equal weight ───────────────────────────────────────────────────────
inline WeightVec equal_weight(int n) {
    return WeightVec(n, 1.0/n);
}

// ── 2. Inverse volatility ─────────────────────────────────────────────────
inline WeightVec inverse_vol(const CovMatrix& cov) {
    int n = (int)cov.size();
    WeightVec w(n);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        double vol = std::sqrt(std::max(cov[i][i], 0.0));
        w[i] = (vol > 1e-9) ? 1.0/vol : 0;
        sum += w[i];
    }
    if (sum > 0) for (auto& x : w) x /= sum;
    return w;
}

// ── 3. Risk parity (equal risk contribution) ──────────────────────────────
inline WeightVec risk_parity(const CovMatrix& cov, int max_iter = 300) {
    int n = (int)cov.size();
    WeightVec w(n, 1.0/n);

    for (int it = 0; it < max_iter; ++it) {
        double pv = port_var(w, cov);
        if (pv < 1e-16) break;
        double pstd = std::sqrt(pv);
        double target = pstd / n;
        double max_err = 0;

        for (int i = 0; i < n; ++i) {
            double mrc = 0;  // marginal risk contribution
            for (int j = 0; j < n; ++j) mrc += w[j]*cov[i][j];
            double rc  = w[i] * mrc / pstd;
            double err = rc - target;
            max_err    = std::max(max_err, std::abs(err));
            w[i]       *= std::max(1 - 0.3*err/pstd, 0.01);
            w[i]        = std::max(w[i], 1e-6);
        }
        double s = 0; for (double x : w) s += x;
        for (double& x : w) x /= s;
        if (max_err < 1e-8) break;
    }
    return w;
}

// ── 4. Minimum variance ───────────────────────────────────────────────────
inline WeightVec min_variance(const CovMatrix& cov, int max_iter = 500,
                               double lr = 0.01) {
    int n = (int)cov.size();
    WeightVec w = inverse_vol(cov);  // warm start

    for (int it = 0; it < max_iter; ++it) {
        // Gradient: dVar/dwi = 2 * (Σw)_i
        WeightVec grad(n, 0);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                grad[i] += 2 * cov[i][j] * w[j];

        // Projected gradient step
        for (int i = 0; i < n; ++i) w[i] -= lr * grad[i];
        w = normalize(w);
    }
    return w;
}

// ── 5. Maximum Sharpe (tangency portfolio) ────────────────────────────────
inline WeightVec max_sharpe(const CovMatrix& cov, const ReturnVec& mu,
                              double rf = 0.05, int max_iter = 500) {
    int n = (int)cov.size();
    WeightVec w = equal_weight(n);

    for (int it = 0; it < max_iter; ++it) {
        double pr  = port_ret(w, mu);
        double pv  = port_var(w, cov);
        if (pv < 1e-16) break;
        double psd = std::sqrt(pv);
        double sh  = (pr - rf) / psd;

        // Gradient of Sharpe w.r.t. w
        WeightVec grad(n);
        for (int i = 0; i < n; ++i) {
            double dret_dwi = mu[i];
            double mrc = 0;
            for (int j = 0; j < n; ++j) mrc += cov[i][j]*w[j];
            double dvar_dwi = 2*mrc;
            grad[i] = (dret_dwi*psd - (pr-rf)*dvar_dwi/(2*psd)) / pv;
        }

        double lr = 0.01;
        for (int i = 0; i < n; ++i) w[i] += lr * grad[i];
        w = normalize(w);
    }
    return w;
}

// ── 6. Hierarchical Risk Parity (Lopez de Prado, 2016) ────────────────────
// Clusters assets by correlation, then allocates via bisection
inline WeightVec hrp(const CovMatrix& cov) {
    int n = (int)cov.size();
    if (n == 0) return {};
    if (n == 1) return {1.0};

    // Build correlation matrix
    std::vector<std::vector<double>> corr(n, std::vector<double>(n, 0));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            corr[i][j] = (cov[i][i]*cov[j][j] > 0)
                ? cov[i][j]/std::sqrt(cov[i][i]*cov[j][j]) : 0;

    // Ward's linkage-style distance: distance = sqrt(0.5*(1-corr))
    auto dist = [&](int i, int j) {
        return std::sqrt(0.5 * (1 - corr[i][j]));
    };

    // Simple quasi-diagonalization (greedy nearest-neighbour)
    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    for (int i = 0; i < n-1; ++i) {
        int best_j = i+1;
        double min_d = dist(order[i], order[i+1]);
        for (int j = i+2; j < n; ++j) {
            double d = dist(order[i], order[j]);
            if (d < min_d) { min_d = d; best_j = j; }
        }
        std::swap(order[i+1], order[best_j]);
    }

    // Recursive bisection
    WeightVec w(n, 1.0);
    std::function<void(int,int)> bisect = [&](int lo, int hi) {
        if (hi - lo <= 1) return;
        int mid = (lo + hi) / 2;
        bisect(lo, mid);
        bisect(mid, hi);

        // Compute cluster variances
        auto cluster_var = [&](int a, int b) {
            // inverse vol weight within cluster
            int sz = b - a;
            double cv = 0;
            for (int i = a; i < b; ++i) {
                double vi = std::sqrt(std::max(cov[order[i]][order[i]], 0.0));
                if (vi > 0) cv += cov[order[i]][order[i]] / (vi*vi);
            }
            return cv / (sz*sz);
        };

        double v1 = cluster_var(lo, mid);
        double v2 = cluster_var(mid, hi);
        double tot = v1 + v2;
        double alpha = (tot > 0) ? v2/tot : 0.5;  // weight cluster 1 by v2/(v1+v2)

        for (int i = lo;  i < mid; ++i) w[order[i]] *= alpha;
        for (int i = mid; i < hi;  ++i) w[order[i]] *= (1-alpha);
    };

    bisect(0, n);
    return normalize(w);
}

// ── 7. Black-Litterman scaffold ───────────────────────────────────────────
// Blends equilibrium returns with manager views
struct BLView {
    std::vector<double> portfolio;   // asset weights in view portfolio
    double              view_return; // expected return of view portfolio
    double              confidence;  // variance of view (lower = more confident)
};

inline ReturnVec black_litterman(
    const CovMatrix& cov,
    const WeightVec& mkt_cap_weights,
    const std::vector<BLView>& views,
    double risk_aversion = 2.5,
    double tau = 0.05)
{
    int n = (int)cov.size();
    // Equilibrium returns: Π = δ * Σ * w_mkt
    ReturnVec pi(n, 0);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            pi[i] += risk_aversion * cov[i][j] * mkt_cap_weights[j];

    if (views.empty()) return pi;

    // Build P matrix and Q vector
    int k = (int)views.size();
    std::vector<std::vector<double>> P(k, std::vector<double>(n, 0));
    std::vector<double> Q(k), omega(k);
    for (int v = 0; v < k; ++v) {
        for (int i = 0; i < n; ++i) P[v][i] = views[v].portfolio[i];
        Q[v]     = views[v].view_return;
        omega[v] = views[v].confidence;
    }

    // BL formula: μ_BL = [(τΣ)^{-1} + P^T Ω^{-1} P]^{-1}
    //                     [(τΣ)^{-1}Π + P^T Ω^{-1}Q]
    // Simplified 1-view case for illustration
    ReturnVec mu_bl = pi;
    for (int v = 0; v < k; ++v) {
        double omega_inv = (omega[v] > 0) ? 1.0/omega[v] : 0;
        double ptp = 0;  // P_v^T * tau*Sigma * P_v
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                ptp += P[v][i] * tau * cov[i][j] * P[v][j];

        double blend = (ptp + 1.0/omega_inv > 0)
                       ? tau*ptp / (ptp + omega[v]) : 0;

        for (int i = 0; i < n; ++i)
            mu_bl[i] += blend * P[v][i] * (Q[v] - /* Π_v */ 0);
    }
    return mu_bl;
}

} // namespace construction
} // namespace ql
