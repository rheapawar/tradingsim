// Drives OrderBook with randomized order flow and prints a timestamped
// event log, using the Timestamp already recorded on Order/Trade in
// orderbook.cpp (placeOrder sets order.timestamp; matchHelper stamps every
// Trade) — this file is just the first place that actually displays them.
//
// Usage: ./simulate [numEvents]

#include "../orderbook.hpp"

#include <random>
#include <thread>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <vector>
#include <string>

static const char* sideStr(Side s) {
    return s == Side::bid ? "BID" : "ASK";
}

int main(int argc, char** argv) {
    size_t numEvents = 50;
    if (argc > 1) numEvents = strtoul(argv[1], nullptr, 10);

    OrderBook book("TEST", 100.0);
    std::mt19937 rng(std::random_device{}());

    // Prices clustered around the 100.00 reference so orders actually cross
    // each other instead of just piling up at the edges of the 85-115 band.
    std::uniform_real_distribution<double> priceDist(97.00, 103.00);
    std::uniform_int_distribution<int> qtyDist(1, 50);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> actionDist(0, 9); // 0-7 place, 8 cancel, 9 modify
    std::uniform_int_distribution<int> delayDist(1, 5);  // ms between arrivals

    const Timestamp simStart = chrono::steady_clock::now();
    std::vector<uint64_t> restingIds;

    auto elapsedMs = [&](Timestamp t) {
        return chrono::duration_cast<chrono::microseconds>(t - simStart).count() / 1000.0;
    };
    auto log = [&](Timestamp t, const std::string &line) {
        std::cout << "[" << std::fixed << std::setprecision(3) << std::setw(9)
                   << elapsedMs(t) << "ms] " << line << "\n";
    };

    size_t rejected = 0;

    for (size_t i = 0; i < numEvents; ++i) {
        std::this_thread::sleep_for(chrono::milliseconds(delayDist(rng)));

        int action = actionDist(rng);

        if (action == 8 && !restingIds.empty()) {
            std::uniform_int_distribution<size_t> pick(0, restingIds.size() - 1);
            size_t idx = pick(rng);
            uint64_t id = restingIds[idx];
            restingIds.erase(restingIds.begin() + idx);

            bool wasResting = book.hasRestingOrder(id);
            book.cancelOrder(id);
            log(chrono::steady_clock::now(),
                "CANCEL  #" + std::to_string(id) +
                (wasResting ? "" : " (already filled by someone else, safe no-op)"));
            continue;
        }

        if (action == 9 && !restingIds.empty()) {
            std::uniform_int_distribution<size_t> pick(0, restingIds.size() - 1);
            size_t idx = pick(rng);
            uint64_t id = restingIds[idx];
            double newPrice = std::round(priceDist(rng) * 100) / 100.0;
            uint32_t newQty = qtyDist(rng);

            size_t tradesBefore = book.getTrades().size();
            book.modifyOrder(id, newPrice, newQty);
            log(chrono::steady_clock::now(),
                "MODIFY  #" + std::to_string(id) + " -> price=" +
                std::to_string(newPrice) + " qty=" + std::to_string(newQty));

            if (!book.hasRestingOrder(id)) {
                restingIds.erase(std::remove(restingIds.begin(), restingIds.end(), id), restingIds.end());
            }
            for (size_t t = tradesBefore; t < book.getTrades().size(); ++t) {
                const Trade &tr = book.getTrades()[t];
                log(tr.timestamp, "  TRADE  " + std::to_string(tr.quantity) + " @ " +
                    std::to_string(tr.price) + " (buy #" + std::to_string(tr.buyOrderid) +
                    ", sell #" + std::to_string(tr.sellOrderid) + ")");
            }
            continue;
        }

        Order o{};
        o.side = sideDist(rng) ? Side::ask : Side::bid;
        o.price = std::round(priceDist(rng) * 100) / 100.0;
        o.quantity = qtyDist(rng);
        o.ticker = "SIM";

        size_t tradesBefore = book.getTrades().size();
        auto result = book.placeOrder(o);

        log(o.timestamp, std::string("PLACE   ") + sideStr(o.side) + " " +
            std::to_string(o.quantity) + " @ " + std::to_string(o.price) +
            " -> #" + std::to_string(o.orderId) +
            (result == OrderBook::OrderResult::Accepted ? "" : " (REJECTED: invalid price)"));

        if (result != OrderBook::OrderResult::Accepted) {
            ++rejected;
        } else if (book.hasRestingOrder(o.orderId)) {
            restingIds.push_back(o.orderId);
        }

        for (size_t t = tradesBefore; t < book.getTrades().size(); ++t) {
            const Trade &tr = book.getTrades()[t];
            log(tr.timestamp, "  TRADE  " + std::to_string(tr.quantity) + " @ " +
                std::to_string(tr.price) + " (buy #" + std::to_string(tr.buyOrderid) +
                ", sell #" + std::to_string(tr.sellOrderid) + ")");
        }
    }

    std::cout << "\n--- summary ---\n";
    std::cout << "events processed: " << numEvents << "\n";
    std::cout << "orders rejected:  " << rejected << "\n";
    std::cout << "trades executed:  " << book.getTrades().size() << "\n";
    std::cout << "still resting:    " << restingIds.size() << "\n";
    std::cout << "run took:         " << elapsedMs(chrono::steady_clock::now()) << "ms\n";

    return 0;
}
