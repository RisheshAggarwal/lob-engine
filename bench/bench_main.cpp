// Benchmarks:
//  1. Single-threaded matching cost per message (limit / market / cancel mix).
//  2. End-to-end pipeline: a producer thread ("gateway") sends orders through the
//     lock-free SPSC queue to a consumer thread ("matching engine"). We measure
//     per-message latency from enqueue to "match done" and report percentiles.
//
// Numbers depend on the machine. macOS does not allow pinning threads to cores,
// so tail latencies here are noisier than on a tuned Linux box.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/spsc_queue.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

enum class MsgType : std::uint8_t { Limit, Market, Cancel, Stop };

struct Msg {
  MsgType type;
  Side side;
  Price price;
  Qty qty;
  OrderId id;
  std::int64_t sent_ns;  // when the producer enqueued it
};

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

constexpr Price kMin = 90'000;
constexpr Price kMax = 110'000;
constexpr Price kMid = 100'000;

// Order flow: 70% limit orders clustered around the mid price (some cross the
// spread), 10% market orders, 20% cancels of previously added orders.
std::vector<Msg> make_flow(std::size_t n, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int> kind(0, 99);
  std::normal_distribution<double> offset(0.0, 15.0);
  std::uniform_int_distribution<Qty> qty(1, 100);
  std::vector<Msg> out;
  out.reserve(n);
  std::vector<OrderId> live;
  OrderId next_id = 1;
  for (std::size_t i = 0; i < n; ++i) {
    const int k = kind(rng);
    Msg m{};
    if (k < 70) {
      m.type = MsgType::Limit;
      m.side = (rng() & 1) ? Side::Buy : Side::Sell;
      const int off = static_cast<int>(offset(rng));
      m.price = m.side == Side::Buy ? kMid - 2 + off : kMid + 2 + off;
      m.price = std::clamp(m.price, kMin, kMax);
      m.qty = qty(rng);
      m.id = next_id++;
      live.push_back(m.id);
    } else if (k < 80) {
      m.type = MsgType::Market;
      m.side = (rng() & 1) ? Side::Buy : Side::Sell;
      m.qty = qty(rng);
      m.id = next_id++;
    } else {
      m.type = MsgType::Cancel;  // may target an already-filled order; that is fine
      if (live.empty()) {
        m.id = 0;
      } else {
        std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
        const std::size_t j = pick(rng);
        m.id = live[j];
        live[j] = live.back();
        live.pop_back();
      }
    }
    out.push_back(m);
  }
  return out;
}

struct TradeCounter {
  std::uint64_t count = 0;
  std::uint64_t volume = 0;
  void operator()(const Trade& t) {
    ++count;
    volume += t.qty;
  }
};

void apply(OrderBook& book, const Msg& m, TradeCounter& trades) {
  switch (m.type) {
    case MsgType::Limit:
      book.add_limit(m.id, m.side, m.price, m.qty, trades);
      break;
    case MsgType::Market:
      book.add_market(m.id, m.side, m.qty, trades);
      break;
    case MsgType::Cancel:
      book.cancel(m.id);
      break;
    case MsgType::Stop:
      break;
  }
}

std::int64_t pct(std::vector<std::int64_t>& v, double p) {
  const std::size_t i = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<long>(i), v.end());
  return v[i];
}

void bench_single_thread(const std::vector<Msg>& flow) {
  OrderBook book(kMin, kMax, flow.size());
  TradeCounter trades;
  const auto t0 = Clock::now();
  for (const Msg& m : flow) apply(book, m, trades);
  const auto t1 = Clock::now();
  const double ns = static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  std::printf("[single-thread] %zu msgs in %.1f ms -> %.1f ns/msg (%.2f M msgs/s), %llu trades\n",
              flow.size(), ns / 1e6, ns / static_cast<double>(flow.size()),
              static_cast<double>(flow.size()) / (ns / 1e9) / 1e6,
              static_cast<unsigned long long>(trades.count));
}

void bench_pipeline(const std::vector<Msg>& flow, std::int64_t pace_ns) {
  static SpscQueue<Msg, 1 << 16> queue;  // static: ~2 MB buffer, keep it off the stack
  std::vector<std::int64_t> lat;
  lat.reserve(flow.size());

  std::thread engine([&] {
    OrderBook book(kMin, kMax, flow.size());
    TradeCounter trades;
    Msg m{};
    for (;;) {
      if (!queue.try_pop(m)) continue;  // busy-spin: lowest latency, uses a full core
      if (m.type == MsgType::Stop) break;
      apply(book, m, trades);
      lat.push_back(now_ns() - m.sent_ns);
    }
  });

  const auto t0 = Clock::now();
  for (Msg m : flow) {
    m.sent_ns = now_ns();
    while (!queue.try_push(m)) {
    }
    // Pace the producer so we measure latency rather than queue backlog.
    const auto spin_until = Clock::now() + std::chrono::nanoseconds(pace_ns);
    while (Clock::now() < spin_until) {
    }
  }
  Msg stop{};
  stop.type = MsgType::Stop;
  while (!queue.try_push(stop)) {
  }
  engine.join();
  const auto t1 = Clock::now();

  const double secs =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()) /
      1e9;
  std::printf("[pipeline] %zu msgs in %.2f s (producer paced at 1 msg / %lld ns)\n", lat.size(),
              secs, static_cast<long long>(pace_ns));
  const std::int64_t mx = *std::max_element(lat.begin(), lat.end());
  std::printf("[pipeline] enqueue->matched latency: p50 %lld ns | p99 %lld ns | p99.9 %lld ns | max %lld ns\n",
              static_cast<long long>(pct(lat, 0.50)), static_cast<long long>(pct(lat, 0.99)),
              static_cast<long long>(pct(lat, 0.999)), static_cast<long long>(mx));
}

}  // namespace

int main(int argc, char** argv) {
  // Usage: bench [num_messages] [producer_pace_ns]
  std::size_t n = 2'000'000;
  std::int64_t pace_ns = 1000;  // 1M msgs/s offered load
  if (argc > 1) n = static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10));
  if (argc > 2) pace_ns = std::strtoll(argv[2], nullptr, 10);
  std::printf("Generating %zu messages...\n", n);
  const std::vector<Msg> flow = make_flow(n, /*seed*/ 42);
  bench_single_thread(flow);
  bench_pipeline(flow, pace_ns);
  return 0;
}
