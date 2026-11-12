#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

namespace lob {

// Bounded lock-free single-producer / single-consumer ring buffer.
//
// Exactly one thread may call try_push() and exactly one (other) thread may
// call try_pop(). No mutex is used; the two threads synchronise only through
// the head_/tail_ atomics.
//
// Key ideas (good to be able to explain):
//  * Capacity is a power of two, so `index & mask_` replaces a slow modulo.
//  * head_ and tail_ are on separate cache lines (alignas(64)) to avoid false
//    sharing: the producer writes tail_, the consumer writes head_, and if they
//    shared a cache line every write would bounce the line between CPU cores.
//  * Each side keeps a *cached* copy of the other side's index and only reloads
//    the real atomic when the cached value says the queue looks full/empty.
//    This cuts cross-core traffic dramatically.
//  * Memory ordering: the producer writes the slot, then publishes it with a
//    release store to tail_. The consumer's acquire load of tail_ guarantees it
//    sees the slot contents. The same pairing in reverse on head_ lets the
//    producer safely reuse a slot.
template <class T, std::size_t Capacity>
class SpscQueue {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");
  static_assert(std::is_trivially_copyable<T>::value, "T should be trivially copyable");

 public:
  bool try_push(const T& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only we write tail_
    if (tail - head_cache_ == Capacity) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == Capacity) return false;  // really full
    }
    buffer_[tail & kMask] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const std::size_t head = head_.load(std::memory_order_relaxed);  // only we write head_
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return false;  // really empty
    }
    out = buffer_[head & kMask];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  static constexpr std::size_t capacity() { return Capacity; }

 private:
  static constexpr std::size_t kMask = Capacity - 1;
  static constexpr std::size_t kCacheLine = 64;

  // Consumer-owned.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  alignas(kCacheLine) std::size_t tail_cache_ = 0;
  // Producer-owned.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  alignas(kCacheLine) std::size_t head_cache_ = 0;

  alignas(kCacheLine) T buffer_[Capacity];
};

}  // namespace lob
