#include <string>
#include <vector>
#include <map>
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

        unordered_map<uint64_t, list<Order>::iterator> orderLookup;

        int pricetoIndex(double price) const{
            return static_cast<int>(round((price - LOWER_BOUND)/TICK_SIZE));
        }

    public:
        OrderBook(){
            bid_orders.resize(NUM_TICKS);
            ask_orders.resize(NUM_TICKS);
        }

        void addOrder(Order &order){
            int index = pricetoIndex(order.price);
            price_level &lvl = (order.side == Side::ask) ? ask_orders[index] : bid_orders[index];
            
            auto it = lvl.orders.insert(lvl.orders.end(), order);
            orderLookup[order.orderId] = it;
        }

        void cancelOrder(Order &order){
            int index = pricetoIndex(order.price);
            price_level &lvl = (order.side == Side::ask) ? ask_orders[index] : bid_orders[index];

            auto it = orderLookup[order.orderId];
            lvl.orders.erase(it);
            auto it2 = orderLookup.find(order.orderId);
            orderLookup.erase(it2);
        }

        bool matchOrder(Order &order){
            int index = pricetoIndex(order.price);
            if(order.side == Side::ask && bestBidIndex >= index){
                while(order.quantity > 0 && bestBidIndex >= index){
                    price_level* lvl = &bid_orders[bestBidIndex];
                    if(lvl->orders.empty()){
                        --bestBidIndex;
                        continue;
                    }
                    while(!lvl->orders.empty() && lvl->orders.front().quantity <= order.quantity && order.quantity > 0){
                        order.quantity -= lvl->orders.front().quantity;
                        cancelOrder(lvl->orders.front());
                    }
                    if(!lvl->orders.empty() && order.quantity > 0){
                        lvl->orders.front().quantity -= order.quantity;
                        order.quantity = 0;
                    }
                }
                return true;
            }
            if(order.side == Side::bid && bestAskIndex <= index){
                while(order.quantity > 0 && bestBidIndex <= index){
                    price_level* lvl = &ask_orders[bestAskIndex];
                    if(lvl->orders.empty()){
                        ++bestAskIndex;
                        continue;
                    }
                    while(!lvl->orders.empty() && lvl->orders.front().quantity <= order.quantity && order.quantity > 0){
                        order.quantity -= lvl->orders.front().quantity;
                        cancelOrder(lvl->orders.front());
                    }
                    if(!lvl->orders.empty() && order.quantity > 0){
                        lvl->orders.front().quantity -= order.quantity;
                        order.quantity = 0;
                    }
                }
                return true;
            }
            return false;
        }
};

