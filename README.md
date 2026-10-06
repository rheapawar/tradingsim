# tradingsim

An in-memory limit order book matching engine written in C++. Each `OrderBook` is scoped to one symbol and one reference price, matches incoming orders against resting ones by price-time (FIFO) priority, and rests whatever doesn't fill.

## Features

- **Limit orders** — rest on the book if they don't fully match; fill against the best available opposing price otherwise.
- **Cancellation** — id-based; looks the order up by id and removes it directly from wherever it's actually resting, rather than trusting caller-supplied price/side.
- **Modify (cancel/replace)** — a quantity *decrease* at the same price edits the resting order in place and keeps its queue position; a price change or quantity *increase* removes it and re-inserts at the back of the (possibly new) level, since it should lose time priority. Either way the order keeps its original id, and a modify to a price outside the band is rejected without touching the resting order.
- **Price-time priority** — best price matches first; FIFO within a price level.
- **Bounded price band** — each book only accepts prices within ±15% of its reference price, mapped onto a fixed grid of 1-cent ticks.

## Architecture

There's no threading or queueing here — everything is synchronous, single-threaded:

```
caller ──placeOrder()──▶ submitOrder() ──▶ matchOrder() ──▶ matchHelper() walks price levels, best price first
          (validates price,      │
           assigns new id)       └─▶ addOrder() rests whatever quantity is left over

caller ──modifyOrder()──▶ cancelOrder() + submitOrder()   (price change / size increase; keeps the id)
```

`cancelOrder` goes straight through `orderLookup` to the order's actual position in the book rather than through the matching path. `modifyOrder` validates the new price, removes the order with `cancelOrder`, then re-submits it through `submitOrder` under the same id, so it can match if the new price crosses and otherwise rests at the back of its level.

### OrderBook internals

| Structure | Type | Purpose |
|---|---|---|
| `bid_orders` / `ask_orders` | `vector<price_level>` (`price_level` wraps a `list<Order>`) | One FIFO queue of resting orders per price tick, indexed directly by `pricetoIndex(price)` — O(1) lookup, no tree/heap. |
| `orderLookup` | `unordered_map<uint64_t, list<Order>::iterator>` | Order id → its exact node in whichever `list<Order>` it's resting in. The single source of truth `cancelOrder`/`modifyOrder` use. |
| `bidBits` / `askBits` | `vector<uint64_t>` | Occupancy bitmap, one bit per tick: set while that price level has at least one resting order. |
| `bestBidIndex` / `bestAskIndex` | `int` | Tick index of the best resting price on each side; `-1` means that side is empty. Always points at a non-empty level. |
| `executedTrades` | `vector<Trade>` | Every fill, in execution order. |
| `tradesbyId` | `unordered_map<uint64_t, vector<size_t>>` | Order id → indices into `executedTrades` involving it, for both full and partial fills. Queried with `getTradesForOrder`. |

## Matching

`matchOrder` walks price levels from the best available opposing price outward, only as far as the incoming order's own limit price allows, stopping once the order is fully filled or nothing left crosses. `matchHelper` handles one price level: it consumes whole resting orders front-to-back (FIFO), and on the last one, either fills the incoming order completely (leaving a smaller resting order behind) or fills the resting order completely (whichever runs out first).

### Keeping the best price current

Because the book has a fixed grid of ticks per side (3,000 for a $100 reference price) and most of them are empty at any moment, the best-price index has to skip straight to the next live level rather than step through empty ones tick by tick. Each side keeps an occupancy bitmap (`bidBits` / `askBits`) with one bit per tick:

- `addOrder` sets the level's bit.
- When `cancelOrder` empties a level (including when a fill removes its last order), it clears the bit. If that level was the best price, it moves the best index to the next set bit: `prevSet` (highest set bit at or below) for bids, `nextSet` (lowest set bit at or above) for asks.
- `prevSet` / `nextSet` scan 64 ticks per word, using `__builtin_clzll` / `__builtin_ctzll` to find the set bit within a word in one instruction.

`matchOrder` never moves the best index itself; it just keeps matching at whatever level the index points to until the incoming order is filled or the best price no longer crosses.

## Performance

Benchmarked by replaying one deterministic stream of 2M operations (80% new orders, 10% cancels, 10% modifies, random prices and sizes 1–50) through three books with identical semantics, and checking that all three produce exactly the same trades:

- **Before:** this engine without the occupancy bitmap, where the best-price index lags behind cancels and fills.
- **Current:** this engine with the occupancy bitmap.
- **`std::map` levels:** the same engine with price levels stored in a red-black tree instead of a flat array.

Full band (prices spread across the whole ±15% range, so most levels are empty), median of 5 runs:

| Book | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| Before (lagging best index) | 2.3M ops/s | 160 ns | 2.0 µs | 4.4–5.6 µs |
| **Current (occupancy bitmap)** | **3.0M ops/s** | **140 ns** | **1.1 µs** | **2.9–3.2 µs** |
| `std::map` levels | 2.7M ops/s | 180 ns | 1.2 µs | 3.2–3.5 µs |

The bitmap cut p99 latency by about 45% and puts the flat-array book ahead of the tree-based one. On a tight band (prices within ±3%, so few empty levels) the bitmap still has the best throughput (3.0–3.2M ops/s) and median (120–130 ns), but all three books are within about 0.15 µs of each other at p99.

Latency is per operation and includes the cost of reading the clock. Numbers are from an Intel Core Ultra 7 255U under WSL2 (g++ 13, `-O2 -march=native`) and will vary by machine.

## API

```cpp
OrderBook book("AAPL", 100.0);   // ticker + reference price; band = reference price ± 15%

Order o(Side::bid, "AAPL", 99.50, 10);
auto result = book.placeOrder(o);        // Accepted / RejectedInvalidPrice

book.cancelOrder(o.orderId);
book.modifyOrder(o.orderId, 99.75, 15);  // OrderModified / RejectedInvalidPrice; o.orderId stays valid

book.getTrades();                         // const vector<Trade>&, all fills so far
book.getTradesForOrder(o.orderId);        // vector<Trade>, fills involving this order
book.hasRestingOrder(o.orderId);
```

## Build & Run

```sh
# the engine's own tiny demo
g++ -std=c++17 -O2 orderbook.cpp -o orderbook_demo && ./orderbook_demo

# test suite
g++ -std=c++17 -g tests/test_orderbook.cpp -o tests/test_orderbook && ./tests/test_orderbook

# same tests, instrumented for use-after-free / UB / leaks
g++ -std=c++17 -g -fsanitize=address,undefined,leak -fno-omit-frame-pointer tests/test_orderbook.cpp -o tests/test_orderbook_asan && ./tests/test_orderbook_asan

# benchmark: current book vs. pre-bitmap book vs. std::map book (args: ops, runs; default 2000000 5)
g++ -std=c++17 -O2 -march=native benchmark/bench.cpp -o benchmark/bench && ./benchmark/bench

# randomized simulation (numEvents optional, defaults to 50)
g++ -std=c++17 -O2 sim/simulate.cpp -o sim/simulate -pthread && ./sim/simulate 100
```

`tests/test_orderbook.cpp` includes `orderbook.hpp` directly and drives `OrderBook` only through its public API (`placeOrder`/`cancelOrder`/`modifyOrder`/`getTrades`/...). Each test runs in its own forked child process so a crash in one doesn't take out the rest of the suite. It covers price-band boundaries, resting/zero-quantity orders, cancel (including unknown-id and double-cancel), matching (partial fill, exact fill, multi-level sweep, per-order trade tracking), and modify (quantity-only, price change, unknown id, id kept after modify, loss of queue priority on price change and size increase, rejected modify leaves the order resting).

## Work to come

- No market, IOC, FOK, or stop orders — limit orders only.
- Not thread-safe — no locking around the book's internal state; fine for a single-threaded driver, not for concurrent submission from multiple threads.
- `Order::sequenceNumber` is declared but never assigned — FIFO within a level currently comes for free from `list` insertion order, but a real sequence number would let fills be reconstructed in strict global arrival order across price levels, not just within one.
- One `OrderBook` per symbol is manual right now — no registry/manager routing an incoming order to the right book by ticker.
- No persistence or replay — the whole book lives in memory and disappears when the process exits.
