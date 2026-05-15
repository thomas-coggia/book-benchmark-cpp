#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <thread>

#include "matching/runtime/agent.hpp"
#include "matching/runtime/agent_system.hpp"
#include "matching/runtime/event_loop.hpp"
#include "matching/runtime/spsc_queue.hpp"

namespace matching::runtime {

  namespace {

    /// Trivially-constructible flag carried on the test queues. Two alternatives are encoded
    /// by an integer payload + a sentinel bool — kept this simple so the test does not
    /// depend on matching::input_event_t.
    struct probe_event_t {
      int value{0};
      bool sentinel{false};
    };

    /// Counts non-sentinel events; returns @c true on a sentinel to terminate the loop.
    struct counting_handler_t {
      std::atomic<std::size_t>* counter{nullptr};

      [[nodiscard]] bool operator()(const probe_event_t& event) noexcept {
        if (event.sentinel) {
          return true;
        }
        counter->fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    };

  }  // namespace

  TEST(AgentTest, GracefulShutdownViaSentinel) {
    using queue_t = spsc_queue_t<probe_event_t, 1u << 12>;
    queue_t queue;
    std::atomic<std::size_t> counter{0};
    std::stop_source source;

    auto loop = make_event_loop(
      queue_source_t<queue_t>{queue},
      counting_handler_t{&counter},
      source.get_token()
    );
    auto agent = make_agent(std::move(loop), std::nullopt);

    agent.start();

    constexpr int n = 1000;
    for (int i = 0; i < n; ++i) {
      while (!queue.try_push(probe_event_t{i, false})) {
        cpu_pause();
      }
    }
    while (!queue.try_push(probe_event_t{0, true})) {
      cpu_pause();
    }

    agent.join();

    EXPECT_EQ(counter.load(std::memory_order_acquire), static_cast<std::size_t>(n));
  }

  TEST(AgentTest, AbortViaStopSource) {
    using queue_t = spsc_queue_t<probe_event_t, 1u << 12>;
    queue_t queue;
    std::atomic<std::size_t> counter{0};
    std::stop_source source;

    auto loop = make_event_loop(
      queue_source_t<queue_t>{queue},
      counting_handler_t{&counter},
      source.get_token()
    );
    auto agent = make_agent(std::move(loop), std::nullopt);

    agent.start();
    // No events pushed: the agent is busy-waiting in cpu_pause(). Request stop and verify
    // the join completes within a generous deadline.
    const auto t0 = std::chrono::steady_clock::now();
    source.request_stop();
    agent.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_LT(elapsed, std::chrono::seconds{2});
    EXPECT_EQ(counter.load(std::memory_order_acquire), 0u);
  }

  TEST(AgentSystemTest, ChainedFlushPropagatesThroughTwoStages) {
    using queue_t = spsc_queue_t<probe_event_t, 1u << 12>;
    queue_t queue_a;
    queue_t queue_b;
    std::atomic<std::size_t> downstream_counter{0};
    std::stop_source source;
    const auto token = source.get_token();

    // Stage 1: pop from queue_a, forward to queue_b. Sentinel forwards a sentinel and exits.
    struct forwarding_handler_t {
      queue_t* downstream{nullptr};
      std::stop_token token;

      [[nodiscard]] bool operator()(const probe_event_t& event) noexcept {
        if (event.sentinel) {
          while (!token.stop_requested() && !downstream->try_push(probe_event_t{0, true})) {
            cpu_pause();
          }
          return true;
        }
        while (!token.stop_requested() && !downstream->try_push(event)) {
          cpu_pause();
        }
        return false;
      }
    };

    auto fwd_loop = make_event_loop(
      queue_source_t<queue_t>{queue_a},
      forwarding_handler_t{&queue_b, token},
      token
    );
    auto fwd_agent = make_agent(std::move(fwd_loop), std::nullopt);

    auto cnt_loop = make_event_loop(
      queue_source_t<queue_t>{queue_b},
      counting_handler_t{&downstream_counter},
      token
    );
    auto cnt_agent = make_agent(std::move(cnt_loop), std::nullopt);

    auto system = make_agent_system(source, std::move(fwd_agent), std::move(cnt_agent));
    system.start();

    constexpr int n = 500;
    for (int i = 0; i < n; ++i) {
      while (!queue_a.try_push(probe_event_t{i, false})) {
        cpu_pause();
      }
    }
    while (!queue_a.try_push(probe_event_t{0, true})) {
      cpu_pause();
    }

    system.join();

    EXPECT_EQ(downstream_counter.load(std::memory_order_acquire), static_cast<std::size_t>(n));
  }

}  // namespace matching::runtime
