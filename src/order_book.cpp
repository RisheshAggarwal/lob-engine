#include "lob/order_book.hpp"

#include <stdexcept>

namespace lob {

OrderBook::OrderBook(Price min_price, Price max_price, std::size_t max_orders)
    : min_price_(min_price),
      max_price_(max_price),
      bids_(),
      asks_(),
      best_bid_(-1),
      best_ask_(0),
      pool_(max_orders) {
  if (max_price < min_price) throw std::invalid_argument("max_price < min_price");
  const std::size_t n = static_cast<std::size_t>(max_price - min_price) + 1;
  bids_.resize(n);
  asks_.resize(n);
  best_ask_ = static_cast<long>(n);  // "no asks" sentinel
  index_.reserve(max_orders);       // avoid rehashing on the hot path
}

void OrderBook::append(Level& lvl, Order* o) {
  o->prev = lvl.tail;
  o->next = nullptr;
  if (lvl.tail != nullptr) {
    lvl.tail->next = o;
  } else {
    lvl.head = o;
  }
  lvl.tail = o;
  lvl.total += o->qty;
}

void OrderBook::unlink(Level& lvl, Order* o) {
  if (o->prev != nullptr) {
    o->prev->next = o->next;
  } else {
    lvl.head = o->next;
  }
  if (o->next != nullptr) {
    o->next->prev = o->prev;
  } else {
    lvl.tail = o->prev;
  }
  o->prev = o->next = nullptr;
}

void OrderBook::refresh_best_bid() {
  while (best_bid_ >= 0 && bids_[static_cast<std::size_t>(best_bid_)].head == nullptr) --best_bid_;
}

void OrderBook::refresh_best_ask() {
  const long n = static_cast<long>(asks_.size());
  while (best_ask_ < n && asks_[static_cast<std::size_t>(best_ask_)].head == nullptr) ++best_ask_;
}

bool OrderBook::cancel(OrderId id) {
  auto it = index_.find(id);
  if (it == index_.end()) return false;
  Order* o = it->second;
  Level& lvl = levels(o->side)[idx(o->price)];
  lvl.total -= o->qty;
  unlink(lvl, o);
  index_.erase(it);
  const Side side = o->side;
  pool_.release(o);
  if (lvl.head == nullptr) {
    if (side == Side::Buy) {
      refresh_best_bid();
    } else {
      refresh_best_ask();
    }
  }
  return true;
}

std::optional<Price> OrderBook::best_bid() const {
  if (best_bid_ < 0) return std::nullopt;
  return price_of(best_bid_);
}

std::optional<Price> OrderBook::best_ask() const {
  if (best_ask_ >= static_cast<long>(asks_.size())) return std::nullopt;
  return price_of(best_ask_);
}

Qty OrderBook::depth_at(Side side, Price price) const {
  if (!in_range(price)) return 0;
  return levels(side)[idx(price)].total;
}

}  // namespace lob
