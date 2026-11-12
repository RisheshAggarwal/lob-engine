#pragma once

#include <cstdint>

namespace lob {

// Prices are integer ticks (e.g. 1 tick = 0.01). Integers avoid floating-point
// rounding errors and make price comparison and indexing cheap.
using Price = std::int32_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

// One execution between a resting (maker) order and an incoming (taker) order.
// Trades always happen at the maker's price (price-time priority).
struct Trade {
  OrderId maker_id;
  OrderId taker_id;
  Price price;
  Qty qty;
};

enum class Status : std::uint8_t {
  Ok,
  InvalidQty,       // qty == 0
  PriceOutOfRange,  // price outside the book's configured tick range
  DuplicateId,      // an order with this id is already resting
  PoolExhausted,    // no free order slots left (book is full)
};

struct AddResult {
  Status status;
  Qty filled;   // quantity executed immediately against resting orders
  Qty resting;  // quantity left resting on the book (always 0 for market orders)
};

}  // namespace lob
