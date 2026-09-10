# tradingsim

An in-memory limit order book matching engine written in C++. Each `OrderBook` is scoped to one symbol and one reference price, matches incoming orders against resting ones by price-time (FIFO) priority, and rests whatever doesn't fill.

## Features

- **Limit orders** — rest on the book if they don't fully match; fill against the best available opposing price otherwise.
- **Cancellation** — id-based; looks the order up by id and removes it directly from wherever it's actually resting, rather than trusting caller-supplied price/side.
- **Modify (cancel/replace)** — a quantity *decrease* at the same price edits the resting order in place and keeps its queue position; a price change or quantity *increase* removes it and re-inserts at the back of the (possibly new) level, since it should lose time priority.
- **Price-time priority** — best price matches first; FIFO within a price level.
- **Bounded price band** — each book only accepts prices within ±15% of its reference price, mapped onto a fixed grid of 1-cent ticks.

## Architecture

There's no threading or queueing here — everything is synchronous, single-threaded:

```
caller ──placeOrder()──▶ matchOrder() ──▶ matchHelper() walks price levels, best price first
                              │
                              └─▶ addOrder() rests whatever quantity is left over
```

`cancelOrder`/`modifyOrder` go straight through `orderLookup` to the order's actual position in the book rather than through the matching path.

### OrderBook internals

| Structure | Type | Purpose |
|---|---|---|
| `bid_orders` / `ask_orders` | `vector<price_level>` (`price_level` wraps a `list<Order>`) | One FIFO queue of resting orders per price tick, indexed directly by `pricetoIndex(price)` — O(1) lookup, no tree/heap. |
| `orderLookup` | `unordered_map<uint64_t, list<Order>::iterator>` | Order id → its exact node in whichever `list<Order>` it's resting in. The single source of truth `cancelOrder`/`modifyOrder` use. |
| `bestBidIndex` / `bestAskIndex` | `int` | Tick index of the best resting price on each side; `-1` means that side is empty. |
| `executedTrades` | `vector<Trade>` | Every fill, in execution order. |
| `tradesbyId` | `unordered_map<uint64_t, vector<size_t>>` | Order id → indices into `executedTrades` involving it. Populated on every fill; not yet exposed through a public API. |

## Matching

`matchOrder` walks price levels from the best available opposing price outward, only as far as the incoming order's own limit price allows, stopping once the order is fully filled or nothing left crosses. `matchHelper` handles one price level: it consumes whole resting orders front-to-back (FIFO), and on the last one, either fills the incoming order completely (leaving a smaller resting order behind) or fills the resting order completely (whichever runs out first).

## API

```cpp
OrderBook book("AAPL", 100.0);   // ticker + reference price; band = reference price ± 15%

Order o(Side::bid, "AAPL", 99.50, 10);
auto result = book.placeOrder(o);        // Accepted / RejectedInvalidPrice

book.cancelOrder(o.orderId);
book.modifyOrder(o.orderId, 99.75, 15);  // OrderModified / RejectedInvalidPrice

book.getTrades();                         // const vector<Trade>&, all fills so far
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

# randomized simulation (numEvents optional, defaults to 50)
g++ -std=c++17 -O2 sim/simulate.cpp -o sim/simulate -pthread && ./sim/simulate 100
```

`tests/test_orderbook.cpp` includes `orderbook.hpp` directly and drives `OrderBook` only through its public API (`placeOrder`/`cancelOrder`/`modifyOrder`/`getTrades`/...). Each test runs in its own forked child process so a crash in one doesn't take out the rest of the suite. It covers price-band boundaries, resting/zero-quantity orders, cancel (including unknown-id and double-cancel), matching (partial fill, exact fill, multi-level sweep), and modify (quantity-only, price change, unknown id).

## Work to come

- No market, IOC, FOK, or stop orders — limit orders only.
- Not thread-safe — no locking around the book's internal state; fine for a single-threaded driver, not for concurrent submission from multiple threads.
- `tradesbyId` is populated on every fill but there's no public API yet to query "all trades for order X."
- `Order::sequenceNumber` is declared but never assigned — FIFO within a level currently comes for free from `list` insertion order, but a real sequence number would let fills be reconstructed in strict global arrival order across price levels, not just within one.
- One `OrderBook` per symbol is manual right now — no registry/manager routing an incoming order to the right book by ticker.
- No persistence or replay — the whole book lives in memory and disappears when the process exits.
