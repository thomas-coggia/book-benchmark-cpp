#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace matching::runtime {

  /// Bounded lock-free single-producer / single-consumer ring buffer.
  ///
  /// Capacity is a compile-time power of two so wrap-around uses a single mask AND.
  /// Producer- and consumer-owned indices live on their own cache lines (alignas(64)) so the
  /// hot pushes from one core do not invalidate the line the other core reads its index from.
  /// Memory ordering follows the textbook SPSC release/acquire pairing: a successful @c
  /// try_push publishes the slot write with a release store on @c head_; the matching @c
  /// try_pop sees the new head with an acquire load before reading the slot.
  ///
  /// Element type @c T must be trivially destructible — the slot is overwritten in-place on
  /// every push, so destructors are never called. The buffer is heap-allocated through
  /// @c std::aligned_alloc to keep the queue's own object footprint small and stack-safe
  /// even at large capacities.
  template <typename T, std::size_t Capacity>
  class spsc_queue_t {
    static_assert(Capacity >= 2, "Capacity must be at least 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_destructible_v<T>, "T must be trivially destructible");

  public:
    using value_type = T;

    spsc_queue_t() {
      constexpr std::size_t cache_line = 64;
      const std::size_t bytes = ((Capacity * sizeof(T) + cache_line - 1) / cache_line) * cache_line;
      void* const raw = std::aligned_alloc(cache_line, bytes);
      if (raw == nullptr) {
        throw std::bad_alloc{};
      }
      slots_ = static_cast<T*>(raw);
      for (std::size_t i = 0; i < Capacity; ++i) {
        new (slots_ + i) T{};
      }
    }

    ~spsc_queue_t() noexcept {
      // T is trivially destructible (enforced above), so no per-element destructor calls.
      std::free(slots_);
    }

    spsc_queue_t(const spsc_queue_t&) = delete;
    spsc_queue_t& operator=(const spsc_queue_t&) = delete;
    spsc_queue_t(spsc_queue_t&&) = delete;
    spsc_queue_t& operator=(spsc_queue_t&&) = delete;

    /// Producer-side push. Returns @c false when the queue is full. Producer reads its own
    /// @c head_ relaxed (it is the sole writer) and the @c tail_ acquire (to detect a full
    /// ring). The slot write happens before the @c head_ release store.
    [[nodiscard]] bool try_push(const T& value) noexcept {
      const std::size_t head = head_.load(std::memory_order_relaxed);
      const std::size_t next = (head + 1) & mask_v;
      if (next == tail_.load(std::memory_order_acquire)) {
        return false;
      }
      slots_[head] = value;
      head_.store(next, std::memory_order_release);
      return true;
    }

    /// Consumer-side pop. Returns @c false when the queue is empty. Consumer reads its own
    /// @c tail_ relaxed and the @c head_ acquire (to see the producer's slot write). The
    /// slot read happens before the @c tail_ release store.
    [[nodiscard]] bool try_pop(T& out) noexcept {
      const std::size_t tail = tail_.load(std::memory_order_relaxed);
      if (tail == head_.load(std::memory_order_acquire)) {
        return false;
      }
      out = slots_[tail];
      tail_.store((tail + 1) & mask_v, std::memory_order_release);
      return true;
    }

    /// Approximate occupancy. Snapshots are racy under concurrent traffic; useful only for
    /// diagnostics or for an unstrict "near-full" check on the producer side.
    [[nodiscard]] std::size_t size() const noexcept {
      const std::size_t head = head_.load(std::memory_order_acquire);
      const std::size_t tail = tail_.load(std::memory_order_acquire);
      return (head - tail) & mask_v;
    }

    [[nodiscard]] bool empty() const noexcept {
      return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
      return Capacity;
    }

  private:
    static constexpr std::size_t mask_v = Capacity - 1;
    static constexpr std::size_t cache_line_v = 64;

    alignas(cache_line_v) std::atomic<std::size_t> head_{0};
    alignas(cache_line_v) std::atomic<std::size_t> tail_{0};
    alignas(cache_line_v) T* slots_{nullptr};
  };

  /// Adapter exposing an @ref spsc_queue_t as a @c Source for @c event_loop_t. Holds a raw
  /// pointer to the queue (the queue lives in the binary's outer scope and is referenced —
  /// not owned — by every agent that touches it).
  template <typename Queue>
  class queue_source_t {
  public:
    using value_type = typename Queue::value_type;

    explicit queue_source_t(Queue& q) noexcept : queue_(&q) {}

    [[nodiscard]] bool try_pop(value_type& out) noexcept {
      return queue_->try_pop(out);
    }

  private:
    Queue* queue_{nullptr};
  };

  /// Same as @ref queue_source_t but shares ownership of the queue via @c std::shared_ptr.
  /// Use when the queue is referenced from multiple agents (e.g. producer + consumer).
  template <typename Queue>
  class queue_source_shared_t {
  public:
    using value_type = typename Queue::value_type;

    explicit queue_source_shared_t(std::shared_ptr<Queue> q) noexcept : queue_(std::move(q)) {}

    [[nodiscard]] bool try_pop(value_type& out) noexcept {
      return queue_->try_pop(out);
    }

  private:
    std::shared_ptr<Queue> queue_;
  };

}  // namespace matching::runtime
