#pragma once
// Same semantics as OrderBook, but price levels live in std::map (red-black tree)
// keyed by tick index, the "textbook" design. Everything else is identical:
// same list<Order> FIFO per level, same id->iterator hash map, same trade recording.
#include "../orderbook.hpp"
#include <map>

class MapOrderBook{
    static constexpr double TICK_SIZE = 0.01;
    static constexpr double BAND_PERCENT = 0.15;
    string ticker; double LOWER_BOUND; int NUM_TICKS;
    map<int, list<Order>, greater<int>> bids;   // best bid = begin()
    map<int, list<Order>> asks;                 // best ask = begin()
    uint64_t nextOrderId = 1;
    unordered_map<uint64_t, list<Order>::iterator> orderLookup;
    vector<Trade> executedTrades;
    unordered_map<uint64_t, vector<size_t>> tradesbyId;
    int pricetoIndex(double p) const { return static_cast<int>(round((p - LOWER_BOUND)/TICK_SIZE)); }

    void addOrder(Order &o){
        int idx = pricetoIndex(o.price);
        list<Order> &lvl = (o.side==Side::ask) ? asks[idx] : bids[idx];
        orderLookup[o.orderId] = lvl.insert(lvl.end(), o);
    }
    void eraseResting(unordered_map<uint64_t, list<Order>::iterator>::iterator found){
        auto it = found->second; int idx = pricetoIndex(it->price);
        if(it->side==Side::ask){ auto m = asks.find(idx); m->second.erase(it); if(m->second.empty()) asks.erase(m); }
        else { auto m = bids.find(idx); m->second.erase(it); if(m->second.empty()) bids.erase(m); }
        orderLookup.erase(found);
    }
    // returns true if level emptied
    bool matchHelper(list<Order> &lvl, Order &order){
        while(!lvl.empty() && order.quantity > 0){
            Order &front = lvl.front();
            if(front.quantity > order.quantity) break;
            size_t index = executedTrades.size();
            order.quantity -= front.quantity;
            (order.side == Side::ask) ? executedTrades.push_back(Trade{front.orderId, order.orderId, front.price, front.quantity, chrono::steady_clock::now()})
                                      : executedTrades.push_back(Trade{order.orderId, front.orderId, front.price, front.quantity, chrono::steady_clock::now()});
            tradesbyId[order.orderId].push_back(index);
            tradesbyId[front.orderId].push_back(index);
            orderLookup.erase(front.orderId);
            lvl.pop_front();
        }
        if(!lvl.empty() && order.quantity > 0){
            Order &front = lvl.front();
            size_t pidx = executedTrades.size();
            front.quantity -= order.quantity;
            tradesbyId[order.orderId].push_back(pidx);
            tradesbyId[front.orderId].push_back(pidx);
            (order.side == Side::ask) ? executedTrades.push_back(Trade{front.orderId, order.orderId, front.price, order.quantity, chrono::steady_clock::now()})
                                      : executedTrades.push_back(Trade{order.orderId, front.orderId, front.price, order.quantity, chrono::steady_clock::now()});
            order.quantity = 0;
        }
        return lvl.empty();
    }
    void matchOrder(Order &order){
        int idx = pricetoIndex(order.price);
        if(order.side==Side::ask){
            while(order.quantity>0 && !bids.empty() && bids.begin()->first >= idx)
                if(matchHelper(bids.begin()->second, order)) bids.erase(bids.begin());
        } else {
            while(order.quantity>0 && !asks.empty() && asks.begin()->first <= idx)
                if(matchHelper(asks.begin()->second, order)) asks.erase(asks.begin());
        }
    }
public:
    MapOrderBook(string t, double ref) : ticker(std::move(t)), LOWER_BOUND(ref*(1-BAND_PERCENT)),
        NUM_TICKS(static_cast<int>((2*ref*BAND_PERCENT)/TICK_SIZE)) {}
    OrderBook::OrderResult placeOrder(Order &order){
        int idx = pricetoIndex(order.price);
        if(idx < 0 || idx >= NUM_TICKS) return OrderBook::OrderResult::RejectedInvalidPrice;
        order.orderId = nextOrderId++;
        order.timestamp = chrono::steady_clock::now();
        matchOrder(order);
        if(order.quantity > 0) addOrder(order);
        return OrderBook::OrderResult::Accepted;
    }
    void cancelOrder(uint64_t id){ auto f = orderLookup.find(id); if(f != orderLookup.end()) eraseResting(f); }
    OrderBook::OrderResult modifyOrder(uint64_t id, double price, uint32_t qty){
        auto f = orderLookup.find(id);
        if(f == orderLookup.end()) return OrderBook::OrderResult::RejectedInvalidPrice;
        auto it = f->second;
        if(it->price != price || it->quantity < qty){
            int ni = pricetoIndex(price);
            if(ni < 0 || ni >= NUM_TICKS) return OrderBook::OrderResult::RejectedInvalidPrice;
            Order changed = *it; eraseResting(f);
            changed.price = price; changed.quantity = qty;
            changed.timestamp = chrono::steady_clock::now();
            matchOrder(changed); if(changed.quantity > 0) addOrder(changed);
            return OrderBook::OrderResult::OrderModified;
        }
        it->quantity = qty; return OrderBook::OrderResult::OrderModified;
    }
    const vector<Trade>& getTrades() const { return executedTrades; }
};
