// Black-box tests for OrderBook, driven only through its public API.
// Each test runs in its own forked child process so that a crash in one
// test (there are a couple of real ones below) doesn't take out the rest
// of the suite.

#include "../orderbook.hpp"

#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <vector>
#include <string>
#include <functional>

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            exit(1); \
        } \
    } while (0)

static Order makeOrder(Side side, double price, uint32_t qty) {
    Order o{};
    o.side = side;
    o.price = price;
    o.quantity = qty;
    o.ticker = "TST";
    o.sequenceNumber = 0;
    return o;
}

// ---- price band edges: LOWER_BOUND = 100*(1-0.15) = 85.00, first invalid
// price above the band is 115.00 (index math mirrored from orderbook.cpp) ----

void test_reject_price_below_band() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::bid, 84.99, 10);
    CHECK(book.placeOrder(o) == OrderBook::OrderResult::RejectedInvalidPrice);
}

void test_reject_price_at_upper_bound() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::ask, 115.00, 10);
    CHECK(book.placeOrder(o) == OrderBook::OrderResult::RejectedInvalidPrice);
}

void test_accept_lower_boundary_price() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::bid, 85.00, 10);
    CHECK(book.placeOrder(o) == OrderBook::OrderResult::Accepted);
    CHECK(book.hasRestingOrder(o.orderId));
}

void test_accept_upper_boundary_price() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::ask, 114.99, 10);
    CHECK(book.placeOrder(o) == OrderBook::OrderResult::Accepted);
    CHECK(book.hasRestingOrder(o.orderId));
}

// ---- basic resting / zero-quantity ----

void test_non_crossing_order_rests() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::bid, 99.00, 25);
    book.placeOrder(o);
    CHECK(book.hasRestingOrder(o.orderId));
    CHECK(book.restingQuantity(o) == 25);
    CHECK(book.getTrades().empty());
}

void test_zero_quantity_order_does_not_rest_or_trade() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::bid, 99.00, 0);
    CHECK(book.placeOrder(o) == OrderBook::OrderResult::Accepted);
    CHECK(!book.hasRestingOrder(o.orderId));
    CHECK(book.restingQuantity(o) == 0);
    CHECK(book.getTrades().empty());
}

// ---- cancel ----

void test_cancel_existing_order_removes_it() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::ask, 101.00, 15);
    book.placeOrder(o);
    CHECK(book.hasRestingOrder(o.orderId));
    book.cancelOrder(o.orderId);
    CHECK(!book.hasRestingOrder(o.orderId));
    CHECK(book.restingQuantity(o) == 0);
}

// Edge case requested: cancelling an id that was never placed. A robust
// cancelOrder should reject/no-op this instead of touching the book.
void test_cancel_unknown_order_id_is_a_safe_noop() {
    OrderBook book("TEST", 100.0);
    Order bogus = makeOrder(Side::bid, 90.00, 5);
    bogus.orderId = 999999; // never placed on this book
    book.cancelOrder(bogus.orderId);
    CHECK(!book.hasRestingOrder(999999));
}

// Edge case requested: cancelling the same order twice.
void test_cancel_same_order_twice_is_safe() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::bid, 90.00, 5);
    book.placeOrder(o);
    book.cancelOrder(o.orderId);
    book.cancelOrder(o.orderId); // second cancel on an id already removed
    CHECK(!book.hasRestingOrder(o.orderId));
}

// ---- matching ----

void test_crossing_ask_matches_resting_bid() {
    OrderBook book("TEST", 100.0);
    Order restingBid = makeOrder(Side::bid, 100.00, 10);
    book.placeOrder(restingBid);

    Order aggressiveAsk = makeOrder(Side::ask, 99.00, 10); // crosses the bid
    book.placeOrder(aggressiveAsk);

    CHECK(book.getTrades().size() == 1);
    CHECK(!book.hasRestingOrder(restingBid.orderId));
    CHECK(!book.hasRestingOrder(aggressiveAsk.orderId));
}

void test_crossing_bid_matches_resting_ask() {
    OrderBook book("TEST", 100.0);
    Order restingAsk = makeOrder(Side::ask, 100.00, 10);
    book.placeOrder(restingAsk);

    Order aggressiveBid = makeOrder(Side::bid, 101.00, 10); // crosses the ask
    book.placeOrder(aggressiveBid);

    CHECK(book.getTrades().size() == 1);
    CHECK(!book.hasRestingOrder(restingAsk.orderId));
    CHECK(!book.hasRestingOrder(aggressiveBid.orderId));
}

void test_partial_fill_leaves_remainder_resting() {
    OrderBook book("TEST", 100.0);
    Order restingBid = makeOrder(Side::bid, 100.00, 20);
    book.placeOrder(restingBid);

    Order aggressiveAsk = makeOrder(Side::ask, 100.00, 8);
    book.placeOrder(aggressiveAsk);

    CHECK(book.getTrades().size() == 1);
    CHECK(book.getTrades().back().quantity == 8);
    CHECK(book.hasRestingOrder(restingBid.orderId));
    CHECK(book.restingQuantity(restingBid) == 12);
    CHECK(!book.hasRestingOrder(aggressiveAsk.orderId));
}

void test_exact_quantity_match_removes_both_orders() {
    OrderBook book("TEST", 100.0);
    Order restingBid = makeOrder(Side::bid, 100.00, 10);
    book.placeOrder(restingBid);

    Order aggressiveAsk = makeOrder(Side::ask, 100.00, 10);
    book.placeOrder(aggressiveAsk);

    CHECK(book.getTrades().size() == 1);
    CHECK(book.restingQuantity(restingBid) == 0);
    CHECK(!book.hasRestingOrder(restingBid.orderId));
    CHECK(!book.hasRestingOrder(aggressiveAsk.orderId));
}

// Edge case requested: one order large enough to sweep through and clear
// out several resting price levels entirely.
void test_aggressive_order_clears_entire_book() {
    OrderBook book("TEST", 100.0);
    Order bid1 = makeOrder(Side::bid, 100.00, 10);
    Order bid2 = makeOrder(Side::bid, 99.50, 10);
    Order bid3 = makeOrder(Side::bid, 99.00, 10);
    book.placeOrder(bid1);
    book.placeOrder(bid2);
    book.placeOrder(bid3);

    Order sweep = makeOrder(Side::ask, 99.00, 30); // should consume all three levels
    book.placeOrder(sweep);

    CHECK(book.getTrades().size() == 3);
    CHECK(!book.hasRestingOrder(bid1.orderId));
    CHECK(!book.hasRestingOrder(bid2.orderId));
    CHECK(!book.hasRestingOrder(bid3.orderId));
    CHECK(!book.hasRestingOrder(sweep.orderId));
    CHECK(book.restingQuantity(bid1) == 0);
    CHECK(book.restingQuantity(bid2) == 0);
    CHECK(book.restingQuantity(bid3) == 0);
}

// Aggressive order bigger than the entire resting side: it should sweep
// everything available and then rest with whatever quantity is left over.
void test_aggressive_order_bigger_than_book_rests_remainder() {
    OrderBook book("TEST", 100.0);
    Order bid1 = makeOrder(Side::bid, 100.00, 10);
    Order bid2 = makeOrder(Side::bid, 99.00, 10);
    book.placeOrder(bid1);
    book.placeOrder(bid2);

    Order sweep = makeOrder(Side::ask, 99.00, 25); // 20 available, 5 left over
    book.placeOrder(sweep);

    CHECK(book.getTrades().size() == 2);
    CHECK(!book.hasRestingOrder(bid1.orderId));
    CHECK(!book.hasRestingOrder(bid2.orderId));
    CHECK(book.hasRestingOrder(sweep.orderId));
    CHECK(book.restingQuantity(sweep) == 5);
}

// ---- modify ----

void test_modify_quantity_only_does_not_duplicate_order() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::bid, 95.00, 10);
    book.placeOrder(o);

    book.modifyOrder(o.orderId, 95.00, 20);

    // Exactly one resting order worth 20 at this level, not the original
    // 10 plus a second new order for 20.
    CHECK(book.restingQuantity(o) == 20);
    CHECK(book.restingCount(o) == 1);
}

void test_modify_price_moves_order_between_levels() {
    OrderBook book("TEST", 100.0);
    Order o = makeOrder(Side::bid, 95.00, 10);
    book.placeOrder(o);

    Order oldLevelProbe = makeOrder(Side::bid, 95.00, 0);
    book.modifyOrder(o.orderId, 96.00, 10);

    CHECK(book.restingQuantity(oldLevelProbe) == 0);
    Order newLevelProbe = makeOrder(Side::bid, 96.00, 0);
    CHECK(book.restingQuantity(newLevelProbe) == 10);
}

// Edge case requested: modifying an id the book has never seen.
void test_modify_unknown_order_id_is_rejected_or_noop() {
    OrderBook book("TEST", 100.0);
    Order bogus = makeOrder(Side::bid, 90.00, 5);
    bogus.orderId = 999999; // never placed
    book.modifyOrder(bogus.orderId, 91.00, 5);
    CHECK(!book.hasRestingOrder(999999));
}

int main() {
    std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"reject_price_below_band", test_reject_price_below_band},
        {"reject_price_at_upper_bound", test_reject_price_at_upper_bound},
        {"accept_lower_boundary_price", test_accept_lower_boundary_price},
        {"accept_upper_boundary_price", test_accept_upper_boundary_price},
        {"non_crossing_order_rests", test_non_crossing_order_rests},
        {"zero_quantity_order_does_not_rest_or_trade", test_zero_quantity_order_does_not_rest_or_trade},
        {"cancel_existing_order_removes_it", test_cancel_existing_order_removes_it},
        {"cancel_unknown_order_id_is_a_safe_noop", test_cancel_unknown_order_id_is_a_safe_noop},
        {"cancel_same_order_twice_is_safe", test_cancel_same_order_twice_is_safe},
        {"crossing_ask_matches_resting_bid", test_crossing_ask_matches_resting_bid},
        {"crossing_bid_matches_resting_ask", test_crossing_bid_matches_resting_ask},
        {"partial_fill_leaves_remainder_resting", test_partial_fill_leaves_remainder_resting},
        {"exact_quantity_match_removes_both_orders", test_exact_quantity_match_removes_both_orders},
        {"aggressive_order_clears_entire_book", test_aggressive_order_clears_entire_book},
        {"aggressive_order_bigger_than_book_rests_remainder", test_aggressive_order_bigger_than_book_rests_remainder},
        {"modify_quantity_only_does_not_duplicate_order", test_modify_quantity_only_does_not_duplicate_order},
        {"modify_price_moves_order_between_levels", test_modify_price_moves_order_between_levels},
        {"modify_unknown_order_id_is_rejected_or_noop", test_modify_unknown_order_id_is_rejected_or_noop},
    };

    int passed = 0, failed = 0;
    for (auto &t : tests) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            t.second();
            exit(0); // exit(), not _exit(): must run atexit so LeakSanitizer's check fires
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("[PASS]  %s\n", t.first.c_str());
            passed++;
        } else if (WIFSIGNALED(status)) {
            printf("[CRASH] %s (signal %d: %s)\n", t.first.c_str(), WTERMSIG(status), strsignal(WTERMSIG(status)));
            failed++;
        } else {
            printf("[FAIL]  %s\n", t.first.c_str());
            failed++;
        }
    }
    printf("\n%d passed, %d failed (of %zu)\n", passed, failed, tests.size());
    return failed == 0 ? 0 : 1;
}
