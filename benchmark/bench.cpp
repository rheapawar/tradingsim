// Replays one deterministic order stream through three books with identical semantics:
//   LagOrderBook  - your engine before the bitmap (best index lags behind cancels/fills)
//   OrderBook     - your current engine (occupancy bitmap)
//   MapOrderBook  - same engine, price levels in std::map (red-black tree)
#include "../orderbook.hpp"
#include "lag_orderbook.hpp"
#include "map_orderbook.hpp"
#include <random>
#include <cstdio>
#include <cstdlib>

struct Op { int type; Side side; double price; uint32_t qty; uint64_t id; };

vector<Op> makeOps(size_t n, double lo, double hi, uint64_t seed){
    mt19937_64 rng(seed);
    uniform_real_distribution<double> pd(lo, hi);
    uniform_int_distribution<int> qd(1,50), sd(0,1), ad(0,9);
    vector<Op> ops; ops.reserve(n); uint64_t placed = 0;
    for(size_t i=0;i<n;++i){
        int a = ad(rng); Op op{};
        op.price = round(pd(rng)*100)/100.0; op.qty = qd(rng); op.side = sd(rng)?Side::ask:Side::bid;
        if(a<8 || placed==0){ op.type=0; ++placed; }
        else { op.type = (a==8)?1:2;
               op.id = uniform_int_distribution<uint64_t>(placed>2000?placed-2000:1, placed)(rng); }
        ops.push_back(op);
    }
    return ops;
}

struct Res { double mops; uint32_t p50, p99, p999; vector<Trade> trades; };

template<class Book> Res run(const vector<Op>& ops){
    Book book("TEST", 100.0);
    vector<uint32_t> lat(ops.size());
    auto t0 = chrono::steady_clock::now();
    for(size_t i=0;i<ops.size();++i){
        const Op& op = ops[i];
        auto s = chrono::steady_clock::now();
        if(op.type==0){ Order o(op.side, "TEST", op.price, op.qty); book.placeOrder(o); }
        else if(op.type==1) book.cancelOrder(op.id);
        else book.modifyOrder(op.id, op.price, op.qty);
        lat[i] = (uint32_t)chrono::duration_cast<chrono::nanoseconds>(chrono::steady_clock::now()-s).count();
    }
    double secs = chrono::duration<double>(chrono::steady_clock::now()-t0).count();
    sort(lat.begin(), lat.end());
    auto pct=[&](double p){ return lat[(size_t)(p*(lat.size()-1))]; };
    return {ops.size()/secs/1e6, pct(.5), pct(.99), pct(.999), book.getTrades()};
}

bool same(const vector<Trade>& a, const vector<Trade>& b){
    if(a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i)
        if(a[i].buyOrderid!=b[i].buyOrderid || a[i].sellOrderid!=b[i].sellOrderid || a[i].quantity!=b[i].quantity || a[i].price!=b[i].price) return false;
    return true;
}

template<class T> T med(vector<T> v){ sort(v.begin(), v.end()); return v[v.size()/2]; }

int main(int argc, char** argv){
    size_t n = argc>1 ? strtoull(argv[1],0,10) : 2000000; int reps = argc>2 ? atoi(argv[2]) : 5;
    struct W{ const char* name; double lo, hi; } ws[] = {{"tight band (97-103)",97,103},{"full band (85-115)",85,115}};
    for(auto& w : ws){
        auto ops = makeOps(n, w.lo, w.hi, 42);
        vector<double> m[3]; vector<uint32_t> p50[3], p99[3], p999[3]; bool ok = true; size_t ntr = 0;
        for(int r=0;r<reps;++r){
            Res rs[3] = { run<LagOrderBook>(ops), run<OrderBook>(ops), run<MapOrderBook>(ops) };
            ok = ok && same(rs[0].trades, rs[1].trades) && same(rs[1].trades, rs[2].trades);
            ntr = rs[1].trades.size();
            for(int k=0;k<3;++k){ m[k].push_back(rs[k].mops); p50[k].push_back(rs[k].p50); p99[k].push_back(rs[k].p99); p999[k].push_back(rs[k].p999); }
        }
        const char* names[3] = {"before (lagging best index)","current (occupancy bitmap)","std::map levels"};
        printf("%s: %zu ops x %d runs, %zu trades, identical trades across all books: %s\n", w.name, n, reps, ntr, ok?"yes":"NO");
        for(int k=0;k<3;++k) printf("  %-28s %5.2fM ops/s   p50 %4u ns   p99 %5u ns   p99.9 %6u ns\n",
            names[k], med(m[k]), med(p50[k]), med(p99[k]), med(p999[k]));
    }
}
