#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/order_pool.hpp"
#include "lob/types.hpp"

namespace lob {

// Limit order book with price-time priority matching.
//
// Design (the interesting part for interviews):
//  * Price levels live in a flat array indexed by (price - min_price). Finding a
//    level is O(1) with no tree or hash lookups, and neighbouring prices sit
//    next to each other in memory (cache friendly). The trade-off is that the
//    tick range must be bounded and known up front.
//  * Each level is an intrusive doubly linked FIFO of orders: append at the tail,
//    match from the head (time priority), unlink from the middle in O(1) on cancel.
//  * Orders come from a pre-allocated ObjectPool: no heap allocation while matching.
//  * best_bid_/best_ask_ are cached indices. When a level empties we scan to the
//    next non-empty level; in a real market most activity is near the top of book,
//    so this scan is short in practice.
class OrderBook {
 public:
  OrderBook(Price min_price, Price max_price, std::size_t max_orders);

  // Adds a limit order. Crosses the spread first (filling against resting
  // orders), then rests any remaining quantity on the book.
  // on_trade(const Trade&) is called once per fill.
  template <class OnTrade>
  AddResult add_limit(OrderId id, Side side, Price price, Qty qty, OnTrade&& on_trade);

  // Adds a market order: fills against the best prices available and never rests.
  // Any quantity that cannot be filled is dropped.
  template <class OnTrade>
  AddResult add_market(OrderId id, Side side, Qty qty, OnTrade&& on_trade);

  // Cancels a resting order. Returns false if the id is unknown (for example
  // already fully filled or already cancelled).
  bool cancel(OrderId id);

  std::optional<Price> best_bid() const;
  std::optional<Price> best_ask() const;
  Qty depth_at(Side side, Price price) const;  // total resting qty at a price
  std::size_t resting_orders() const { return index_.size(); }

 private:
  struct Order {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;
    Side side = Side::Buy;
    Order* prev = nullptr;
    Order* next = nullptr;
  };

  struct Level {
    Order* head = nullptr;  // oldest order: matched first
    Order* tail = nullptr;  // newest order
    Qty total = 0;
  };

  std::size_t idx(Price p) const { return static_cast<std::size_t>(p - min_price_); }
  Price price_of(long i) const { return static_cast<Price>(min_price_ + i); }
  bool in_range(Price p) const { return p >= min_price_ && p <= max_price_; }

  std::vector<Level>& levels(Side s) { return s == Side::Buy ? bids_ : asks_; }
  const std::vector<Level>& levels(Side s) const { return s == Side::Buy ? bids_ : asks_; }

  void append(Level& lvl, Order* o);
  void unlink(Level& lvl, Order* o);
  void refresh_best_bid();  // scan down from best_bid_ to the next non-empty level
  void refresh_best_ask();  // scan up from best_ask_ to the next non-empty level

  // Fills `qty` of an incoming order against the opposite side, while the
  // opposite best price is acceptable (`limit` for limit orders; unlimited for
  // market orders). Returns the quantity filled.
  template <class OnTrade>
  Qty match(OrderId taker, Side side, Price limit, bool is_market, Qty qty, OnTrade& on_trade);

  Price min_price_;
  Price max_price_;
  std::vector<Level> bids_;
  std::vector<Level> asks_;
  long best_bid_;  // index of highest non-empty bid level, or -1 when empty
  long best_ask_;  // index of lowest non-empty ask level, or n_levels when empty
  ObjectPool<Order> pool_;
  std::unordered_map<OrderId, Order*> index_;  // id -> resting order, for O(1) cancel
};

// ---------------------------------------------------------------------------
// Template definitions (must be visible to callers).
// ---------------------------------------------------------------------------

template <class OnTrade>
Qty OrderBook::match(OrderId taker, Side side, Price limit, bool is_market, Qty qty,
                     OnTrade& on_trade) {
  const Qty wanted = qty;
  const long n = static_cast<long>(bids_.size());

  if (side == Side::Buy) {
    // A buy takes liquidity from the asks, lowest price first.
    while (qty > 0 && best_ask_ < n && (is_market || price_of(best_ask_) <= limit)) {
      Level& lvl = asks_[static_cast<std::size_t>(best_ask_)];
      while (qty > 0 && lvl.head != nullptr) {
        Order* maker = lvl.head;
        const Qty fill = qty < maker->qty ? qty : maker->qty;
        on_trade(Trade{maker->id, taker, maker->price, fill});
        qty -= fill;
        maker->qty -= fill;
        lvl.total -= fill;
        if (maker->qty == 0) {
          unlink(lvl, maker);
          index_.erase(maker->id);
          pool_.release(maker);
        }
      }
      if (lvl.head == nullptr) refresh_best_ask();
    }
  } else {
    // A sell takes liquidity from the bids, highest price first.
    while (qty > 0 && best_bid_ >= 0 && (is_market || price_of(best_bid_) >= limit)) {
      Level& lvl = bids_[static_cast<std::size_t>(best_bid_)];
      while (qty > 0 && lvl.head != nullptr) {
        Order* maker = lvl.head;
        const Qty fill = qty < maker->qty ? qty : maker->qty;
        on_trade(Trade{maker->id, taker, maker->price, fill});
        qty -= fill;
        maker->qty -= fill;
        lvl.total -= fill;
        if (maker->qty == 0) {
          unlink(lvl, maker);
          index_.erase(maker->id);
          pool_.release(maker);
        }
      }
      if (lvl.head == nullptr) refresh_best_bid();
    }
  }
  return wanted - qty;
}

template <class OnTrade>
AddResult OrderBook::add_limit(OrderId id, Side side, Price price, Qty qty, OnTrade&& on_trade) {
  if (qty == 0) return {Status::InvalidQty, 0, 0};
  if (!in_range(price)) return {Status::PriceOutOfRange, 0, 0};
  if (index_.count(id) != 0) return {Status::DuplicateId, 0, 0};

  const Qty filled = match(id, side, price, /*is_market=*/false, qty, on_trade);
  const Qty remaining = qty - filled;
  if (remaining == 0) return {Status::Ok, filled, 0};

  Order* o = pool_.acquire();
  if (o == nullptr) return {Status::PoolExhausted, filled, 0};
  o->id = id;
  o->price = price;
  o->qty = remaining;
  o->side = side;

  const long i = static_cast<long>(idx(price));
  append(levels(side)[static_cast<std::size_t>(i)], o);
  index_.emplace(id, o);
  if (side == Side::Buy) {
    if (i > best_bid_) best_bid_ = i;
  } else {
    if (i < best_ask_) best_ask_ = i;
  }
  return {Status::Ok, filled, remaining};
}

template <class OnTrade>
AddResult OrderBook::add_market(OrderId id, Side side, Qty qty, OnTrade&& on_trade) {
  if (qty == 0) return {Status::InvalidQty, 0, 0};
  const Qty filled = match(id, side, /*limit=*/0, /*is_market=*/true, qty, on_trade);
  return {Status::Ok, filled, 0};
}

}  // namespace lob
