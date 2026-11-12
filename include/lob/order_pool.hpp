#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// Fixed-capacity object pool.
//
// All objects are allocated once, up front. acquire()/release() are O(1) and
// never touch the heap, so the matching hot path has no malloc/free calls
// (which are slow and have unpredictable latency).
template <class T>
class ObjectPool {
 public:
  explicit ObjectPool(std::size_t capacity) : slots_(capacity) {
    free_.reserve(capacity);
    // Push in reverse so the first acquire() returns slot 0 (nicer for cache).
    for (std::size_t i = capacity; i > 0; --i) free_.push_back(&slots_[i - 1]);
  }

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;

  // Returns nullptr when the pool is exhausted.
  T* acquire() {
    if (free_.empty()) return nullptr;
    T* p = free_.back();
    free_.pop_back();
    return p;
  }

  void release(T* p) { free_.push_back(p); }

  std::size_t capacity() const { return slots_.size(); }
  std::size_t in_use() const { return slots_.size() - free_.size(); }

 private:
  std::vector<T> slots_;  // never resized after construction, so pointers stay valid
  std::vector<T*> free_;  // stack of free slots
};

}  // namespace lob
