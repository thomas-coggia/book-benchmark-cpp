#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>

#include "matching/runtime/spsc_queue.hpp"

namespace matching::runtime {

  TEST(SpscQueueTest, IsEmptyAtConstruction) {
    spsc_queue_t<int, 16> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    int out = -1;
    EXPECT_FALSE(q.try_pop(out));
  }

  TEST(SpscQueueTest, FifoOrderSingleThread) {
    spsc_queue_t<int, 16> q;
    for (int i = 0; i < 10; ++i) {
      EXPECT_TRUE(q.try_push(i));
    }
    EXPECT_EQ(q.size(), 10u);
    for (int i = 0; i < 10; ++i) {
      int out = -1;
      EXPECT_TRUE(q.try_pop(out));
      EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(q.empty());
  }

  TEST(SpscQueueTest, BecomesFullAtCapacityMinusOne) {
    // The "next == tail" full check loses one slot, which is the standard SPSC trade-off.
    constexpr std::size_t cap = 8;
    spsc_queue_t<int, cap> q;
    for (std::size_t i = 0; i < cap - 1; ++i) {
      EXPECT_TRUE(q.try_push(static_cast<int>(i)));
    }
    EXPECT_FALSE(q.try_push(999));
    int out = -1;
    EXPECT_TRUE(q.try_pop(out));
    EXPECT_EQ(out, 0);
    EXPECT_TRUE(q.try_push(999));
  }

  TEST(SpscQueueTest, WrapAroundPreservesOrder) {
    constexpr std::size_t cap = 8;
    spsc_queue_t<int, cap> q;
    int next_pushed = 0;
    int next_popped = 0;
    // Push three, pop three, repeat — exercises wrap-around several times across the
    // capacity boundary while preserving FIFO order.
    for (int round = 0; round < 32; ++round) {
      for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(q.try_push(next_pushed++));
      }
      for (int i = 0; i < 3; ++i) {
        int out = -1;
        EXPECT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, next_popped++);
      }
    }
    EXPECT_TRUE(q.empty());
  }

  TEST(SpscQueueTest, SingleProducerSingleConsumerStress) {
    constexpr std::size_t cap = 1u << 14;
    constexpr std::uint64_t total = 1'000'000;
    spsc_queue_t<std::uint64_t, cap> q;

    std::atomic<std::uint64_t> sum{0};

    std::thread consumer([&] {
      std::uint64_t local = 0;
      std::uint64_t expected = 0;
      while (expected < total) {
        std::uint64_t v = 0;
        if (q.try_pop(v)) {
          // FIFO: every value must arrive in the order it was pushed.
          ASSERT_EQ(v, expected);
          local += v;
          ++expected;
        }
      }
      sum.store(local, std::memory_order_release);
    });

    std::thread producer([&] {
      for (std::uint64_t i = 0; i < total; ++i) {
        while (!q.try_push(i)) {
          // spin
        }
      }
    });

    producer.join();
    consumer.join();

    const std::uint64_t expected_sum = (total - 1) * total / 2;
    EXPECT_EQ(sum.load(std::memory_order_acquire), expected_sum);
  }

  // Compile-time enforcement: power-of-two capacity is required.
  static_assert(std::is_constructible_v<spsc_queue_t<int, 1u << 10>>);

}  // namespace matching::runtime
