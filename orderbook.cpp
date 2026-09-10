#include "orderbook.hpp"

int main(){
    OrderBook book("AAPL", 100.0);

    Order o1(Side::bid, "L1", 100.0, 10);
    Order o2(Side::bid, "L2", 101.0, 5);
    Order o3(Side::ask, "L3", 99.0, 7);
    Order o4(Side::ask, "L4", 100.0, 3);

    book.placeOrder(o1);
    book.placeOrder(o2);
    book.placeOrder(o3);
    book.placeOrder(o4);

    book.cancelOrder(o2.orderId);
    book.modifyOrder(o1.orderId, 100.0, 4);

    for(const auto &t : book.getTrades()){
        cout << t.quantity << " @ " << t.price << " (" << t.buyOrderid << "/" << t.sellOrderid << ")\n";
    }

    return 0;
}
