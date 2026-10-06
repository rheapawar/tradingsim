#pragma once
// OrderBook as it was before the occupancy bitmap: when a cancel or fill empties
// the best level, bestBidIndex/bestAskIndex stay put, and matchOrder steps one
// tick at a time over empty levels. Everything else matches the current OrderBook.
#include "../orderbook.hpp"
class LagOrderBook{
    private:
        struct price_level{
            list<Order> orders;
        };
    
        static constexpr double TICK_SIZE = 0.01;
        static constexpr double BAND_PERCENT = 0.15;

        string ticker;
        double REFERENCE_PRICE;
        double LOWER_BOUND;
        int NUM_TICKS;

        vector<price_level> bid_orders;
        vector<price_level> ask_orders;
        int bestBidIndex =-1;
        int bestAskIndex = -1;
        uint64_t nextOrderId = 1;

        unordered_map<uint64_t, list<Order>::iterator> orderLookup;
        
        vector<Trade> executedTrades;
        unordered_map<uint64_t, vector<size_t>> tradesbyId;

        int pricetoIndex(double price) const{
            return static_cast<int>(round((price - LOWER_BOUND)/TICK_SIZE));
        }

        // match, then rest any remainder at the back of its level; keeps order.orderId
        // callers must validate the price first
        void submitOrder(Order &order){
            order.timestamp = chrono::steady_clock::now();

            matchOrder(order);
            if(order.quantity > 0) addOrder(order);
        }

    public:

        using OrderResult = OrderBook::OrderResult;

        LagOrderBook(string ticker, double referencePrice)
            : ticker(std::move(ticker)),
              REFERENCE_PRICE(referencePrice),
              LOWER_BOUND(referencePrice * (1 - BAND_PERCENT)),
              NUM_TICKS(static_cast<int>((2 * referencePrice * BAND_PERCENT) / TICK_SIZE)){
            bid_orders.resize(NUM_TICKS);
            ask_orders.resize(NUM_TICKS);
        }

        void addOrder(Order &order){
            int index = pricetoIndex(order.price);
            price_level &lvl = (order.side == Side::ask) ? ask_orders[index] : bid_orders[index];
            
            auto it = lvl.orders.insert(lvl.orders.end(), order);
            orderLookup[order.orderId] = it;
            if(order.side == Side::ask) bestAskIndex = (bestAskIndex != -1) ? min(bestAskIndex, index) : index;
            else bestBidIndex = max(bestBidIndex, index);
        }

        void cancelOrder(uint64_t orderId){
            auto found = orderLookup.find(orderId);
            if(found == orderLookup.end()) return;

            auto it = found->second;
            int index = pricetoIndex(it->price);
            price_level &lvl = (it->side == Side::ask) ? ask_orders[index] : bid_orders[index];

            lvl.orders.erase(it);
            orderLookup.erase(found);
        }

        bool matchOrder(Order &order){
            int index = pricetoIndex(order.price);
            if(order.side == Side::ask && bestBidIndex >= index){
                while(order.quantity > 0 && bestBidIndex >= index){
                    if(matchHelper(bid_orders[bestBidIndex], order)) --bestBidIndex;
                }
                return true;
            }
            if(order.side == Side::bid && bestAskIndex >= 0 && bestAskIndex <= index){
                while(order.quantity > 0 && bestAskIndex <= index){
                    if(matchHelper(ask_orders[bestAskIndex], order)) ++bestAskIndex;
                }
                return true;
            }
            
            return false;
        }

        bool matchHelper(price_level &lvl, Order &order){
            while(!lvl.orders.empty() && order.quantity > 0){
                Order &front = lvl.orders.front();
                if(front.quantity > order.quantity) break;

                size_t index = executedTrades.size();
                order.quantity -= front.quantity;
                (order.side == Side::ask) ? executedTrades.push_back(Trade{front.orderId, order.orderId, front.price, front.quantity, chrono::steady_clock::now()}) : 
                                            executedTrades.push_back(Trade{order.orderId, front.orderId, front.price, front.quantity, chrono::steady_clock::now()});
            
                tradesbyId[order.orderId].push_back(index);
                tradesbyId[front.orderId].push_back(index);
                cancelOrder(lvl.orders.front().orderId);
            }
            if(!lvl.orders.empty() && order.quantity > 0){
                Order &front = lvl.orders.front();
                size_t index = executedTrades.size();
                front.quantity -= order.quantity;
                (order.side == Side::ask) ? executedTrades.push_back(Trade{front.orderId, order.orderId, front.price, order.quantity, chrono::steady_clock::now()}) : 
                                            executedTrades.push_back(Trade{order.orderId, front.orderId, front.price, order.quantity, chrono::steady_clock::now()});

                tradesbyId[order.orderId].push_back(index);
                tradesbyId[front.orderId].push_back(index);
                order.quantity = 0;
            }
            return lvl.orders.empty();
        }

        OrderResult placeOrder(Order &order){
            int index = pricetoIndex(order.price);
            if(index < 0 || index >= NUM_TICKS) return OrderResult::RejectedInvalidPrice;

            order.orderId = nextOrderId++;
            submitOrder(order);

            return OrderResult::Accepted;
        }


        OrderResult modifyOrder(uint64_t orderId, double price, uint32_t quantity){
            auto found = orderLookup.find(orderId);
            if(found == orderLookup.end()) return OrderResult::RejectedInvalidPrice;
            auto it = found->second;

            if(it->price != price || it->quantity < quantity){
                // check the new price before touching the book so a rejected modify leaves the order resting
                int index = pricetoIndex(price);
                if(index < 0 || index >= NUM_TICKS) return OrderResult::RejectedInvalidPrice;

                Order changed = *it;
                cancelOrder(orderId);

                changed.price = price;
                changed.quantity = quantity;
                // same orderId, but re-inserted at the back of the queue
                submitOrder(changed);
                return OrderResult::OrderModified;
            }
            else{
                it->quantity = quantity;
                return OrderResult::OrderModified;
            }
        }

        //read-only getters added for testing with no effect on matching behavior

        const vector<Trade>& getTrades() const{
            return executedTrades;
        }

        vector<Trade> getTradesForOrder(uint64_t orderId) const{
            vector<Trade> result;
            auto found = tradesbyId.find(orderId);
            if(found == tradesbyId.end()) return result;
            for(size_t i : found->second) result.push_back(executedTrades[i]);
            return result;
        }

        bool hasRestingOrder(uint64_t orderId) const{
            return orderLookup.count(orderId) > 0;
        }

        uint32_t restingQuantity(Order &search) const{
            int index = pricetoIndex(search.price);
            if(index < 0 || index >= NUM_TICKS) return 0;
            const price_level &lvl = (search.side == Side::ask) ? ask_orders[index] : bid_orders[index];
            uint32_t total = 0;
            for(const auto &o : lvl.orders) total += o.quantity;
            return total;
        }

        size_t restingCount(Order &search) const{
            int index = pricetoIndex(search.price);
            if(index < 0 || index >= NUM_TICKS) return 0;
            const price_level &lvl = (search.side == Side::ask) ? ask_orders[index] : bid_orders[index];
            return lvl.orders.size();
        }
};

