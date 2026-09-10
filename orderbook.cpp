#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <string>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <limits>
#include <iomanip>
#include <cmath>
#include <numeric>
#include <chrono>

using namespace std;

using Timestamp = chrono::time_point<chrono::steady_clock>;

enum class Side{
    bid = 0,
    ask = 1
};

struct Order{
    uint64_t orderId;
    uint64_t sequenceNumber;

    string ticker;
    Side side;

    double price;
    uint32_t quantity;

    Timestamp timestamp;
};

struct Trade{
    uint64_t buyOrderid;
    uint64_t sellOrderid;
    double price;
    uint32_t quantity;
    Timestamp timestamp;
};



class OrderBook{
    private:
        struct price_level{
            list<Order> orders;
        };
    
        static constexpr double TICK_SIZE = 0.01;
        static constexpr double REFERENCE_PRICE = 100.00;
        static constexpr double BAND_PERCENT = 0.15;
        static constexpr double LOWER_BOUND = REFERENCE_PRICE * (1 - BAND_PERCENT);
        static constexpr int NUM_TICKS =
            static_cast<int>((2 * REFERENCE_PRICE * BAND_PERCENT) / TICK_SIZE);

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

    public:

        enum class OrderResult{
            Accepted,
            OrderModified,
            RejectedInvalidPrice
        };

        OrderBook(){
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
                front.quantity -= order.quantity;
                (order.side == Side::ask) ? executedTrades.push_back(Trade{front.orderId, order.orderId, front.price, order.quantity, chrono::steady_clock::now()}) : 
                                            executedTrades.push_back(Trade{order.orderId, front.orderId, front.price, order.quantity, chrono::steady_clock::now()});
               
                order.quantity = 0;
            }
            return lvl.orders.empty();
        }

        OrderResult placeOrder(Order &order){
            int index = pricetoIndex(order.price);
            if(index < 0 || index >= NUM_TICKS) return OrderResult::RejectedInvalidPrice;

            order.orderId = nextOrderId++;
            order.timestamp = chrono::steady_clock::now();

            matchOrder(order);
            if(order.quantity > 0) addOrder(order);

            return OrderResult::Accepted;
        }


        OrderResult modifyOrder(uint64_t orderId, double price, uint32_t quantity){
            auto found = orderLookup.find(orderId);
            if(found == orderLookup.end()) return OrderResult::RejectedInvalidPrice;
            auto it = found->second;

            if(it->price != price || it->quantity < quantity){
                int prev_index = pricetoIndex(it->price);
                price_level &old_lvl = (it->side == Side::ask) ? ask_orders[prev_index] : bid_orders[prev_index];
                Order changed = *it;

                old_lvl.orders.erase(it);
                orderLookup.erase(found);

                changed.price = price;
                changed.quantity = quantity;
                return (placeOrder(changed) == OrderResult::Accepted) ? OrderResult::OrderModified : OrderResult::RejectedInvalidPrice;
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

