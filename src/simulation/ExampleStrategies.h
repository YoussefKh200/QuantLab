#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// src/simulation/ExampleStrategies.h  (MERGED — QuantFusion v1)
// All canonical strategies unified under ql:: namespace.
//
// From QuantLab:   MACrossStrategy, CrossSectionalStrategy,
//                  VolTargetTrendStrategy, MeanReversionStrategy
// From QuantEngine: BreakoutStrategy (Donchian + ATR trailing stop)
//
// Rolling helpers (RollingMean, RollingVol, RollingStd) shared by
// RegimeDetection.h and PortfolioStrategy.h.
// ═══════════════════════════════════════════════════════════════════════════
#include "Backtester.h"
#include <deque>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace ql {

// ── Rolling indicators ─────────────────────────────────────────────────────
class RollingMean {
public:
    explicit RollingMean(int p=10) : p_(p) {}
    void push(double v) {
        q_.push_back(v); sum_+=v;
        if ((int)q_.size()>p_) { sum_-=q_.front(); q_.pop_front(); }
    }
    bool   ready() const { return (int)q_.size()==p_; }
    double value() const { return q_.empty()?0.0:sum_/q_.size(); }
    void   reset(int p) { p_=p; q_.clear(); sum_=0; }
private:
    int p_; double sum_=0; std::deque<double> q_;
};

class RollingVol {
public:
    explicit RollingVol(int p=20, int bpy=252) : p_(p), bpy_(bpy) {}
    void push(double ret) { q_.push_back(ret); if((int)q_.size()>p_) q_.pop_front(); }
    bool   ready() const { return (int)q_.size()==p_; }
    double value() const {
        if ((int)q_.size()<2) return 0;
        double m=0; for (double x:q_) m+=x; m/=q_.size();
        double v=0; for (double x:q_) v+=(x-m)*(x-m);
        return std::sqrt(v/(q_.size()-1)*bpy_);
    }
private:
    int p_, bpy_; std::deque<double> q_;
};

class RollingStd {
public:
    explicit RollingStd(int p=20) : p_(p) {}
    void push(double v) { q_.push_back(v); if((int)q_.size()>p_) q_.pop_front(); }
    bool   ready() const { return (int)q_.size()==p_; }
    double mean() const {
        if (q_.empty()) return 0;
        double m=0; for (double x:q_) m+=x; return m/q_.size();
    }
    double value() const {
        if ((int)q_.size()<2) return 0;
        double m=mean(), v=0;
        for (double x:q_) v+=(x-m)*(x-m);
        return std::sqrt(v/(q_.size()-1));
    }
private:
    int p_; std::deque<double> q_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 1: Moving Average Crossover (vol-scaled, multi-asset)
// ═══════════════════════════════════════════════════════════════════════════
struct MACrossParams {
    int    fast=10, slow=50, vol_window=20;
    double notional=100000.0;
    bool   allow_short=false, size_by_vol=false;
    double target_vol=0.10;
};

class MACrossStrategy : public IStrategy {
public:
    explicit MACrossStrategy(MACrossParams p=MACrossParams()) : p_(p) {}
    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s=sym_[bar.instrument];
        if (!s.init) { s.fast=RollingMean(p_.fast); s.slow=RollingMean(p_.slow);
                        s.vol=RollingVol(p_.vol_window); s.init=true; }
        s.fast.push(bar.close); s.slow.push(bar.close);
        if (s.prev>0) s.vol.push(std::log(bar.close/s.prev));
        s.prev=bar.close;
        if (!s.fast.ready()||!s.slow.ready()) return;
        double fn=s.fast.value(), sn=s.slow.value();
        bool up=fn>sn&&s.pf<=s.ps, dn=fn<=sn&&s.pf>s.ps;
        s.pf=fn; s.ps=sn;
        double not_=p_.notional;
        if (p_.size_by_vol&&s.vol.ready()&&s.vol.value()>0)
            not_ *= p_.target_vol/s.vol.value();
        double qty=std::floor(not_/bar.close);
        if (qty<1) return;
        double cur=position_qty(bar.instrument);
        if (up&&cur<=0) { if(cur<0) buy_market(bar.instrument,-cur); buy_market(bar.instrument,qty); }
        else if (dn&&cur>0) { sell_market(bar.instrument,cur); if(p_.allow_short) sell_market(bar.instrument,qty); }
    }
    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto&[i,_]:sym_) { double q=position_qty(i);
            if(q>0) sell_market(i,q); else if(q<0) buy_market(i,-q); }
    }
private:
    struct Sym { RollingMean fast{10},slow{50}; RollingVol vol{20};
                 double pf=0,ps=0,prev=0; bool init=false; };
    MACrossParams p_; std::unordered_map<InstrumentID,Sym> sym_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 2: Z-Score Mean Reversion with Bollinger Bands
// ═══════════════════════════════════════════════════════════════════════════
struct MeanRevParams {
    int    lookback=20;
    double entry_z=-2.0, exit_z=0.0, stop_z=-4.0, notional=80000.0;
};

class MeanReversionStrategy : public IStrategy {
public:
    explicit MeanReversionStrategy(MeanRevParams p=MeanRevParams()) : p_(p) {}
    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s=sym_[bar.instrument];
        s.stats.push(bar.close);
        if (!s.stats.ready()) return;
        double mu=s.stats.mean(), sd=s.stats.value();
        if (sd<1e-9) return;
        double z=(bar.close-mu)/sd, cur=position_qty(bar.instrument);
        if (cur==0&&z<=p_.entry_z) { double q=std::floor(p_.notional/bar.close); if(q>0) buy_market(bar.instrument,q); }
        else if (cur>0&&(z>=p_.exit_z||z<=p_.stop_z)) sell_market(bar.instrument,cur);
    }
    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto&[i,_]:sym_) { double q=position_qty(i); if(q>0) sell_market(i,q); }
    }
private:
    struct Sym { RollingStd stats{20}; };
    MeanRevParams p_; std::unordered_map<InstrumentID,Sym> sym_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 3: Donchian Channel Breakout + ATR Trailing Stop  (from QE)
// ═══════════════════════════════════════════════════════════════════════════
struct BreakoutParams {
    int channel_period=20, atr_period=14;
    double atr_mult=2.0, notional=80000.0;
};

class BreakoutStrategy : public IStrategy {
public:
    explicit BreakoutStrategy(BreakoutParams p=BreakoutParams()) : p_(p) {}
    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s=sym_[bar.instrument];
        s.highs.push_back(bar.high); s.lows.push_back(bar.low);
        s.atrs.push_back(bar.range());
        if ((int)s.highs.size()>p_.channel_period) s.highs.pop_front();
        if ((int)s.lows.size() >p_.channel_period) s.lows.pop_front();
        if ((int)s.atrs.size() >p_.atr_period)     s.atrs.pop_front();
        if ((int)s.highs.size()<p_.channel_period) return;
        double ch_high=*std::max_element(s.highs.begin(),s.highs.end());
        double atr=0; for(double v:s.atrs) atr+=v; atr/=s.atrs.size();
        double cur=position_qty(bar.instrument);
        if (cur==0&&bar.close>ch_high) {
            double q=std::floor(p_.notional/bar.close);
            if(q>0) { buy_market(bar.instrument,q); s.stop=bar.close-p_.atr_mult*atr; }
        } else if (cur>0) {
            s.stop=std::max(s.stop,bar.close-p_.atr_mult*atr);
            if(bar.low<s.stop) sell_market(bar.instrument,cur);
        }
    }
    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto&[i,_]:sym_) { double q=position_qty(i); if(q>0) sell_market(i,q); }
    }
private:
    struct Sym { std::deque<double> highs,lows,atrs; double stop=0; };
    BreakoutParams p_; std::unordered_map<InstrumentID,Sym> sym_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 4: Volatility-Targeted Trend Following  (from QL)
// ═══════════════════════════════════════════════════════════════════════════
struct VolTargetParams {
    int fast=20, slow=100, vol_window=60;
    double target_vol=0.10, max_leverage=2.0, notional=900000.0;
};

class VolTargetTrendStrategy : public IStrategy {
public:
    explicit VolTargetTrendStrategy(VolTargetParams p=VolTargetParams()) : p_(p) {}
    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& s=sym_[bar.instrument];
        s.fast.push(bar.close); s.slow.push(bar.close);
        if (s.prev>0) s.vol.push(std::log(bar.close/s.prev));
        s.prev=bar.close;
        if (!s.fast.ready()||!s.slow.ready()||!s.vol.ready()) return;
        double trend=s.fast.value()/s.slow.value()-1.0;
        double signal=std::tanh(trend*20.0);
        double scale=std::min(p_.target_vol/s.vol.value(),p_.max_leverage);
        double tgt=std::round(p_.notional*signal*scale/bar.close);
        double delta=tgt-position_qty(bar.instrument);
        if (std::abs(delta)<1) return;
        if (delta>0) buy_market(bar.instrument,delta);
        else         sell_market(bar.instrument,-delta);
    }
    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto&[i,_]:sym_) { double q=position_qty(i);
            if(q>0) sell_market(i,q); else if(q<0) buy_market(i,-q); }
    }
private:
    struct Sym { RollingMean fast{20},slow{100}; RollingVol vol{60}; double prev=0; };
    VolTargetParams p_; std::unordered_map<InstrumentID,Sym> sym_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Strategy 5: Cross-Sectional Factor L/S  (from QL)
// ═══════════════════════════════════════════════════════════════════════════
struct CrossSectionalParams {
    int    rebal_freq=21, lookback=252;
    double long_pct=0.20, short_pct=0.20, gross_notional=1000000.0;
    bool   dollar_neutral=true;
};

class CrossSectionalStrategy : public IStrategy {
public:
    using FactorFn = std::function<double(InstrumentID, const std::deque<Bar>&)>;
    CrossSectionalStrategy(CrossSectionalParams p, FactorFn fn) : p_(p), fn_(std::move(fn)) {}
    void on_bar(const Bar& bar, MarketSimulator& sim, Portfolio& pf) override {
        auto& buf=buf_[bar.instrument];
        buf.push_back(bar);
        if ((int)buf.size()>p_.lookback+10) buf.pop_front();
        if (++bc_%p_.rebal_freq!=0) return;
        std::vector<std::pair<InstrumentID,double>> scores;
        for (auto&[inst,bars]:buf_) {
            if ((int)bars.size()<p_.lookback/2) continue;
            double sc=fn_(inst,bars);
            if (std::isfinite(sc)) scores.push_back({inst,sc});
        }
        if ((int)scores.size()<10) return;
        std::sort(scores.begin(),scores.end(),[](auto&a,auto&b){return a.second>b.second;});
        int n=(int)scores.size(), nl=std::max(1,(int)(n*p_.long_pct)), nb=std::max(1,(int)(n*p_.short_pct));
        double pl=p_.gross_notional*0.5/nl, ps=p_.gross_notional*0.5/nb;
        std::unordered_map<InstrumentID,double> tgt;
        for (int i=0;i<nl;++i)   tgt[scores[i].first]=pl;
        for (int i=n-nb;i<n;++i) tgt[scores[i].first]=-ps;
        for (auto&[inst,tn]:tgt) {
            auto it=buf_.find(inst); if(it==buf_.end()||it->second.empty()) continue;
            double px=it->second.back().close; if(px<=0) continue;
            double tq=std::floor(std::abs(tn)/px)*(tn>0?1:-1);
            double d=tq-position_qty(inst); if(std::abs(d)<1) continue;
            if(d>0) buy_market(inst,d); else sell_market(inst,-d);
        }
        for (auto&[inst,pos]:pf.all_positions()) {
            if(pos.is_flat()||tgt.count(inst)) continue;
            if(pos.quantity>0) sell_market(inst,pos.quantity);
            else               buy_market(inst,-pos.quantity);
        }
    }
    void on_end(MarketSimulator& sim, Portfolio& pf) override {
        for (auto&[inst,pos]:pf.all_positions()) {
            if(pos.is_flat()) continue;
            if(pos.quantity>0) sell_market(inst,pos.quantity);
            else               buy_market(inst,-pos.quantity);
        }
    }
private:
    CrossSectionalParams p_; FactorFn fn_;
    std::unordered_map<InstrumentID,std::deque<Bar>> buf_; int bc_=0;
};

} // namespace ql
