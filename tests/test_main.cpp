// Minimal self-contained test runner (no external framework needed).
#include <cstdio>
#include <thread>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/spsc_queue.hpp"

using namespace lob;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++g_checks;                                                        \
    if (!(cond)) {                                                     \
      ++g_failures;                                                    \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                  \
  } while (0)

#define TEST(name) static void name()
#define RUN(name)                      \
  do {                                 \
    std::printf("- %s\n", #name);      \
    name();                            \
  } while (0)

namespace {

struct TradeLog {
  std::vector<Trade> trades;
  void operator()(const Trade& t) { trades.push_back(t); }
};

OrderBook make_book() { return OrderBook(/*min*/ 9000, /*max*/ 11000, /*max_orders*/ 1024); }

}  // namespace

TEST(empty_book_has_no_best_prices) {
  OrderBook b = make_book();
  CHECK(!b.best_bid().has_value());
  CHECK(!b.best_ask().has_value());
  CHECK(b.resting_orders() == 0);
}

TEST(non_crossing_orders_rest) {
  OrderBook b = make_book();
  TradeLog log;
  auto r1 = b.add_limit(1, Side::Buy, 9990, 10, log);
  auto r2 = b.add_limit(2, Side::Sell, 10010, 5, log);
  CHECK(r1.status == Status::Ok && r1.filled == 0 && r1.resting == 10);
  CHECK(r2.status == Status::Ok && r2.filled == 0 && r2.resting == 5);
  CHECK(log.trades.empty());
  CHECK(b.best_bid() == 9990);
  CHECK(b.best_ask() == 10010);
  CHECK(b.depth_at(Side::Buy, 9990) == 10);
  CHECK(b.depth_at(Side::Sell, 10010) == 5);
}

TEST(full_fill_at_maker_price) {
  OrderBook b = make_book();
  TradeLog log;
  b.add_limit(1, Side::Sell, 10000, 10, log);
  // Aggressive buy at a higher price still trades at the resting (maker) price.
  auto r = b.add_limit(2, Side::Buy, 10005, 10, log);
  CHECK(r.filled == 10 && r.resting == 0);
  CHECK(log.trades.size() == 1);
  CHECK(log.trades[0].maker_id == 1 && log.trades[0].taker_id == 2);
  CHECK(log.trades[0].price == 10000 && log.trades[0].qty == 10);
  CHECK(!b.best_ask().has_value());
  CHECK(b.resting_orders() == 0);
}

TEST(partial_fill_leaves_remainder_resting) {
  OrderBook b = make_book();
  TradeLog log;
  b.add_limit(1, Side::Sell, 10000, 4, log);
  auto r = b.add_limit(2, Side::Buy, 10000, 10, log);
  CHECK(r.filled == 4 && r.resting == 6);
  CHECK(b.best_bid() == 10000);
  CHECK(b.depth_at(Side::Buy, 10000) == 6);
  CHECK(!b.best_ask().has_value());
}

TEST(time_priority_within_a_level) {
  OrderBook b = make_book();
  TradeLog log;
  b.add_limit(1, Side::Sell, 10000, 5, log);  // arrived first
  b.add_limit(2, Side::Sell, 10000, 5, log);  // arrived second
  b.add_limit(3, Side::Buy, 10000, 7, log);
  CHECK(log.trades.size() == 2);
  CHECK(log.trades[0].maker_id == 1 && log.trades[0].qty == 5);
  CHECK(log.trades[1].maker_id == 2 && log.trades[1].qty == 2);
  CHECK(b.depth_at(Side::Sell, 10000) == 3);
}

TEST(price_priority_sweeps_multiple_levels) {
  OrderBook b = make_book();
  TradeLog log;
  b.add_limit(1, Side::Buy, 9998, 5, log);
  b.add_limit(2, Side::Buy, 10000, 5, log);  // best bid
  b.add_limit(3, Side::Buy, 9999, 5, log);
  auto r = b.add_limit(4, Side::Sell, 9999, 12, log);
  // Should hit 10000 first, then 9999, and stop before 9998 (below the limit).
  CHECK(r.filled == 10 && r.resting == 2);
  CHECK(log.trades.size() == 2);
  CHECK(log.trades[0].price == 10000);
  CHECK(log.trades[1].price == 9999);
  CHECK(b.best_bid() == 9998);
  CHECK(b.best_ask() == 9999);  // the unfilled sell remainder now rests at 9999
}

TEST(market_order_never_rests) {
  OrderBook b = make_book();
  TradeLog log;
  b.add_limit(1, Side::Sell, 10000, 3, log);
  b.add_limit(2, Side::Sell, 10050, 3, log);
  auto r = b.add_market(3, Side::Buy, 10, log);
  CHECK(r.filled == 6 && r.resting == 0);
  CHECK(!b.best_ask().has_value());
  CHECK(!b.best_bid().has_value());
  CHECK(b.resting_orders() == 0);
}

TEST(cancel_removes_order_and_updates_best) {
  OrderBook b = make_book();
  TradeLog log;
  b.add_limit(1, Side::Buy, 10000, 5, log);
  b.add_limit(2, Side::Buy, 9990, 5, log);
  CHECK(b.cancel(1));
  CHECK(b.best_bid() == 9990);
  CHECK(!b.cancel(1));    // already gone
  CHECK(!b.cancel(999));  // never existed
  CHECK(b.resting_orders() == 1);
}

TEST(cancel_from_middle_of_queue_keeps_fifo) {
  OrderBook b = make_book();
  TradeLog log;
  b.add_limit(1, Side::Sell, 10000, 1, log);
  b.add_limit(2, Side::Sell, 10000, 1, log);
  b.add_limit(3, Side::Sell, 10000, 1, log);
  CHECK(b.cancel(2));
  b.add_market(4, Side::Buy, 2, log);
  CHECK(log.trades.size() == 2);
  CHECK(log.trades[0].maker_id == 1);
  CHECK(log.trades[1].maker_id == 3);
}

TEST(rejects_invalid_input) {
  OrderBook b = make_book();
  TradeLog log;
  CHECK(b.add_limit(1, Side::Buy, 10000, 0, log).status == Status::InvalidQty);
  CHECK(b.add_limit(1, Side::Buy, 8999, 5, log).status == Status::PriceOutOfRange);
  CHECK(b.add_limit(1, Side::Buy, 11001, 5, log).status == Status::PriceOutOfRange);
  CHECK(b.add_limit(1, Side::Buy, 10000, 5, log).status == Status::Ok);
  CHECK(b.add_limit(1, Side::Buy, 10000, 5, log).status == Status::DuplicateId);
}

TEST(pool_exhaustion_is_reported_not_crashing) {
  OrderBook b(100, 200, /*max_orders*/ 2);
  TradeLog log;
  CHECK(b.add_limit(1, Side::Buy, 150, 1, log).status == Status::Ok);
  CHECK(b.add_limit(2, Side::Buy, 151, 1, log).status == Status::Ok);
  CHECK(b.add_limit(3, Side::Buy, 152, 1, log).status == Status::PoolExhausted);
  CHECK(b.cancel(1));  // frees a slot
  CHECK(b.add_limit(4, Side::Buy, 152, 1, log).status == Status::Ok);
}

TEST(spsc_single_thread_fifo_and_full_empty) {
  SpscQueue<int, 4> q;
  int v = 0;
  CHECK(!q.try_pop(v));
  CHECK(q.try_push(1) && q.try_push(2) && q.try_push(3) && q.try_push(4));
  CHECK(!q.try_push(5));  // full
  CHECK(q.try_pop(v) && v == 1);
  CHECK(q.try_push(5));   // slot freed
  CHECK(q.try_pop(v) && v == 2);
  CHECK(q.try_pop(v) && v == 3);
  CHECK(q.try_pop(v) && v == 4);
  CHECK(q.try_pop(v) && v == 5);
  CHECK(!q.try_pop(v));
}

TEST(spsc_two_threads_preserve_order_and_count) {
  static SpscQueue<std::uint64_t, 1024> q;  // static: keep the buffer off the stack
  constexpr std::uint64_t kN = 2'000'000;
  std::thread producer([] {
    for (std::uint64_t i = 0; i < kN; ++i) {
      while (!q.try_push(i)) {
      }
    }
  });
  std::uint64_t expected = 0;
  bool in_order = true;
  while (expected < kN) {
    std::uint64_t v;
    if (q.try_pop(v)) {
      if (v != expected) in_order = false;
      ++expected;
    }
  }
  producer.join();
  CHECK(in_order);
  CHECK(expected == kN);
}

int main() {
  std::printf("Running tests\n");
  RUN(empty_book_has_no_best_prices);
  RUN(non_crossing_orders_rest);
  RUN(full_fill_at_maker_price);
  RUN(partial_fill_leaves_remainder_resting);
  RUN(time_priority_within_a_level);
  RUN(price_priority_sweeps_multiple_levels);
  RUN(market_order_never_rests);
  RUN(cancel_removes_order_and_updates_best);
  RUN(cancel_from_middle_of_queue_keeps_fifo);
  RUN(rejects_invalid_input);
  RUN(pool_exhaustion_is_reported_not_crashing);
  RUN(spsc_single_thread_fifo_and_full_empty);
  RUN(spsc_two_threads_preserve_order_and_count);
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
