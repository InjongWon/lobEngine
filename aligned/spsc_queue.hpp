#pragma once
#include <array>
#include <atomic>
#include <cstddef>
namespace lob {

// ── Single-Producer Single-Consumer lock-free ring buffer ─────────────────
// Used for order ingestion: network thread pushes, match engine thread pops.
// False-sharing eliminated by padding head/tail to separate cache lines.
//
// Capacity must be a power of two.
template <typename T, std::size_t Capacity> class SPSCQueue {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");
  static constexpr std::size_t MASK = Capacity - 1;

public:
  SPSCQueue() : head_(0), tail_(0) {}

  // Called by producer thread only.
  bool push(const T &item) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t next = (h + 1) & MASK;
    if (next == tail_.load(std::memory_order_acquire))
      return false; // queue full
    buffer_[h] = item;
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Called by consumer thread only. Returns true if item was popped, false if
  // queue was empty.
  bool pop(T &item) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire))
      return false; // queue empty
    item = buffer_[t];
    tail_.store((t + 1) & MASK, std::memory_order_release);
    return true;
  }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

private:
  std::array<T, Capacity> buffer_;

  // Pad to separate cache lines — eliminates false sharing between producer and
  // consumer.
  alignas(64) std::atomic<std::size_t> head_;
  alignas(64) std::atomic<std::size_t> tail_;
};

} // namespace lob