// Interactive order book demo.
//
// Type orders and watch trades happen and the book update. Commands:
//   buy  <qty> <price>   limit buy  (e.g. "buy 10 100")
//   sell <qty> <price>   limit sell (e.g. "sell 5 101")
//   mbuy  <qty>          market buy  (takes the best asks right now)
//   msell <qty>          market sell (takes the best bids right now)
//   cancel <id>          cancel a resting order
//   book                 show the order book
//   sample               load a few sample orders
//   help / quit
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

constexpr Price kMinPrice = 1;
constexpr Price kMaxPrice = 1000;
constexpr int kLevelsShown = 5;

const char* status_text(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::InvalidQty: return "rejected: quantity must be > 0";
    case Status::PriceOutOfRange: return "rejected: price must be between 1 and 1000";
    case Status::DuplicateId: return "rejected: duplicate order id";
    case Status::PoolExhausted: return "rejected: book is full";
  }
  return "?";
}

void print_book(const OrderBook& book) {
  // Collect the best few non-empty levels on each side by scanning prices.
  std::vector<std::pair<Price, Qty>> asks;
  std::vector<std::pair<Price, Qty>> bids;
  if (auto a = book.best_ask()) {
    for (Price p = *a; p <= kMaxPrice && static_cast<int>(asks.size()) < kLevelsShown; ++p) {
      const Qty q = book.depth_at(Side::Sell, p);
      if (q > 0) asks.emplace_back(p, q);
    }
  }
  if (auto b = book.best_bid()) {
    for (Price p = *b; p >= kMinPrice && static_cast<int>(bids.size()) < kLevelsShown; --p) {
      const Qty q = book.depth_at(Side::Buy, p);
      if (q > 0) bids.emplace_back(p, q);
    }
  }

  std::printf("\n          ORDER BOOK\n");
  std::printf("   %-8s %8s\n", "PRICE", "QTY");
  std::printf("  ----------------------\n");
  // Asks printed highest first so the best ask sits just above the spread line.
  for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
    std::printf("   %-8d %8u   SELL%s\n", it->first, it->second,
                (it + 1 == asks.rend()) ? "  <- best ask" : "");
  }
  if (asks.empty()) std::printf("   (no sellers)\n");
  if (auto a = book.best_ask(), b = book.best_bid(); a && b) {
    std::printf("  ------ spread: %d ------\n", *a - *b);
  } else {
    std::printf("  ----------------------\n");
  }
  for (std::size_t i = 0; i < bids.size(); ++i) {
    std::printf("   %-8d %8u   BUY%s\n", bids[i].first, bids[i].second,
                i == 0 ? "   <- best bid" : "");
  }
  if (bids.empty()) std::printf("   (no buyers)\n");
  std::printf("  (%zu resting orders)\n\n", book.resting_orders());
}

void print_help() {
  std::printf(
      "\nCommands:\n"
      "  buy  <qty> <price>   limit buy,  e.g.  buy 10 100\n"
      "  sell <qty> <price>   limit sell, e.g.  sell 5 101\n"
      "  mbuy  <qty>          market buy  (buys from the cheapest sellers now)\n"
      "  msell <qty>          market sell (sells to the highest buyers now)\n"
      "  cancel <id>          cancel a resting order\n"
      "  book                 show the order book\n"
      "  sample               load some sample orders\n"
      "  help                 show this help\n"
      "  quit                 exit\n\n");
}

struct Demo {
  OrderBook book{kMinPrice, kMaxPrice, 10'000};
  OrderId next_id = 1;

  void report(OrderId id, const char* what, const AddResult& r, const std::vector<Trade>& trades) {
    if (r.status != Status::Ok && r.filled == 0) {
      std::printf("  order #%llu %s -> %s\n", static_cast<unsigned long long>(id), what,
                  status_text(r.status));
      return;
    }
    std::printf("  order #%llu %s\n", static_cast<unsigned long long>(id), what);
    for (const Trade& t : trades) {
      std::printf("    TRADE: %u @ %d   (resting #%llu traded with new #%llu)\n", t.qty, t.price,
                  static_cast<unsigned long long>(t.maker_id),
                  static_cast<unsigned long long>(t.taker_id));
    }
    if (trades.empty()) std::printf("    no match\n");
    std::printf("    filled %u", r.filled);
    if (r.resting > 0) std::printf(", %u now resting on the book as #%llu", r.resting,
                                   static_cast<unsigned long long>(id));
    std::printf("\n");
  }

  void limit(Side side, Qty qty, Price price) {
    std::vector<Trade> trades;
    const OrderId id = next_id++;
    const AddResult r =
        book.add_limit(id, side, price, qty, [&](const Trade& t) { trades.push_back(t); });
    char what[64];
    std::snprintf(what, sizeof what, "%s %u @ %d", side == Side::Buy ? "BUY" : "SELL", qty, price);
    report(id, what, r, trades);
  }

  void market(Side side, Qty qty) {
    std::vector<Trade> trades;
    const OrderId id = next_id++;
    const AddResult r = book.add_market(id, side, qty, [&](const Trade& t) { trades.push_back(t); });
    char what[64];
    std::snprintf(what, sizeof what, "MARKET %s %u", side == Side::Buy ? "BUY" : "SELL", qty);
    report(id, what, r, trades);
    if (r.status == Status::Ok && r.filled < qty) {
      std::printf("    (%u could not be filled: not enough on the other side; market orders never rest)\n",
                  qty - r.filled);
    }
  }

  void sample() {
    std::printf("  loading sample orders...\n");
    limit(Side::Sell, 5, 103);
    limit(Side::Sell, 8, 102);
    limit(Side::Sell, 3, 101);
    limit(Side::Sell, 4, 101);
    limit(Side::Buy, 6, 99);
    limit(Side::Buy, 10, 98);
    limit(Side::Buy, 2, 99);
  }
};

}  // namespace

int main() {
  Demo d;
  std::printf("=== Limit Order Book demo ===\n");
  std::printf("Prices are whole numbers from 1 to 1000. Type 'help' for commands,\n");
  std::printf("or 'sample' to load some orders and then try:  buy 5 101\n");
  print_help();

  std::string line;
  while (true) {
    std::printf("> ");
    std::fflush(stdout);
    if (!std::getline(std::cin, line)) break;
    std::istringstream in(line);
    std::string cmd;
    if (!(in >> cmd)) continue;

    if (cmd == "quit" || cmd == "exit" || cmd == "q") break;
    if (cmd == "help" || cmd == "h") {
      print_help();
    } else if (cmd == "book" || cmd == "b") {
      print_book(d.book);
    } else if (cmd == "sample") {
      d.sample();
      print_book(d.book);
    } else if (cmd == "buy" || cmd == "sell") {
      long long qty = 0;
      long long price = 0;
      if (!(in >> qty >> price) || qty <= 0) {
        std::printf("  usage: %s <qty> <price>   e.g. %s 10 100\n", cmd.c_str(), cmd.c_str());
        continue;
      }
      d.limit(cmd == "buy" ? Side::Buy : Side::Sell, static_cast<Qty>(qty),
              static_cast<Price>(price));
      print_book(d.book);
    } else if (cmd == "mbuy" || cmd == "msell") {
      long long qty = 0;
      if (!(in >> qty) || qty <= 0) {
        std::printf("  usage: %s <qty>   e.g. %s 5\n", cmd.c_str(), cmd.c_str());
        continue;
      }
      d.market(cmd == "mbuy" ? Side::Buy : Side::Sell, static_cast<Qty>(qty));
      print_book(d.book);
    } else if (cmd == "cancel") {
      unsigned long long id = 0;
      if (!(in >> id)) {
        std::printf("  usage: cancel <id>   e.g. cancel 3\n");
        continue;
      }
      std::printf(d.book.cancel(id) ? "  cancelled #%llu\n"
                                    : "  #%llu not found (already filled, cancelled, or never existed)\n",
                  id);
      print_book(d.book);
    } else {
      std::printf("  unknown command '%s' (type 'help')\n", cmd.c_str());
    }
  }
  std::printf("bye\n");
  return 0;
}
