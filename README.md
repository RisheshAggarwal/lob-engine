# lob-engine: a low-latency limit order book in C++20

A price-time priority matching engine for a single instrument, with a lock-free
single-producer/single-consumer (SPSC) queue feeding it from a separate thread.
No external dependencies; builds with any C++20 compiler.

## Features

- Limit orders (match first, then rest the remainder), market orders, and cancels
- Price-time priority: best price first, then first-come-first-served within a price
- Trades always execute at the resting (maker) order's price
- O(1) price-level lookup, O(1) cancel, and no heap allocation on the matching path
- Lock-free SPSC ring buffer between a "gateway" thread and the "matching engine" thread
- 13 test cases (65 checks), also run under AddressSanitizer, UBSan and ThreadSanitizer
- Benchmark with latency percentiles (p50 / p99 / p99.9)

## Build and run

```bash
make demo        # interactive: type orders, see trades and the book update
make test        # unit tests
make sanitize    # tests under ASan+UBSan and under TSan (data races)
make bench       # benchmarks: ./build/bench [num_messages] [producer_pace_ns]
```

## Results

Apple Silicon laptop (macOS, Apple clang 21, `-O3`), 2M messages
(70% limit, 10% market, 20% cancel, prices clustered around the mid):

| Benchmark | Result |
|---|---|
| Single-threaded matching | **~33 ns per message** (~30M messages/s) |
| Pipeline, enqueue → matched, at 1M msgs/s offered load | **p50 ≈ 125 ns, p99 ≈ 0.75–1.1 µs** |

p99.9 on macOS is in the milliseconds. This comes from the OS scheduler pausing
the busy-spinning engine thread: macOS has no API to pin a thread to a core or
isolate a core. On Linux you would pin the engine thread (`pthread_setaffinity_np`)
to an isolated core (`isolcpus`), which removes most of that tail. At a faster
producer pace (for example 200 ns/msg) short bursts also back up the queue, so the
benchmark paces the producer to measure latency rather than queueing delay.

## Design

### Order book (`include/lob/order_book.hpp`)

- **Prices are integer ticks**, not floating point, to avoid rounding errors and make
  comparison and indexing cheap.
- **Price levels are a flat array** indexed by `price - min_price`. Looking up a level
  is O(1) with no tree or hash, and neighbouring prices are next to each other in
  memory. The trade-off: the tick range must be bounded and known up front, and
  memory grows with the range rather than with the number of active prices.
- **Each level is an intrusive doubly linked FIFO.** New orders append at the tail,
  matching consumes from the head (time priority), and cancel unlinks from the
  middle in O(1) because each order knows its neighbours.
- **Best bid / best ask are cached indices.** When a level empties, the engine scans
  to the next non-empty level. Most activity is near the top of the book, so the scan
  is usually short; the worst case is O(range).
- **`id → order` hash map** gives O(1) cancel. It is `reserve()`d up front so it never
  rehashes while matching.
- **Object pool** (`order_pool.hpp`): every `Order` is pre-allocated. Acquire/release
  is a pointer push/pop, so the hot path never calls `malloc`/`free`.
- **Trades are reported through a callback template** (`OnTrade`), so the compiler
  can inline it; there is no virtual call or `std::function` overhead.

### SPSC queue (`include/lob/spsc_queue.hpp`)

- Power-of-two capacity, so `index & mask` replaces modulo.
- `head_` (written by the consumer) and `tail_` (written by the producer) sit on
  **separate 64-byte cache lines** to avoid false sharing.
- Each side keeps a **cached copy of the other side's index** and only reloads the
  shared atomic when the queue looks full or empty. This cuts cross-core cache traffic.
- **Memory ordering:** the producer writes the slot, then does a `release` store of
  `tail_`; the consumer's `acquire` load of `tail_` guarantees it sees the slot's data.
  The same pairing on `head_` lets the producer safely reuse a slot.

## Possible next steps

- Pin threads to cores and measure on Linux
- Replace `std::unordered_map` with a flat array keyed by order id (ids are dense)
- Order modify (price/qty change) and IOC/FOK order types
- Multiple instruments, one book per symbol, sharded across engine threads
- A FIX or binary protocol parser in front of the queue
