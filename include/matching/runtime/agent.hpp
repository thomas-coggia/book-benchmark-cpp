#pragma once

#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace matching::runtime {

  namespace detail {

    /// Best-effort CPU pinning. Returns @c true on success, @c false on platform/permission
    /// failure. Failure is non-fatal: the agent runs unpinned, which only affects performance
    /// characteristics, not correctness.
    [[nodiscard]] inline bool pin_current_thread(int cpu_id) noexcept {
#if defined(__linux__)
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(cpu_id, &set);
      return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#else
      (void) cpu_id;
      return false;
#endif
    }

  }  // namespace detail

  /// One worker thread owning an @ref event_loop_t. The contained loop already carries the
  /// system-wide @c std::stop_token (the @c agent_system_t holds a @c std::stop_source sharing
  /// that stop state with every loop at construction), so the agent does not need to
  /// inject anything here — its only responsibilities are starting the thread, optionally
  /// pinning the core, and joining on shutdown.
  ///
  /// @tparam EventLoop A movable type with a @c run() method.
  template <typename EventLoop>
  class agent_t {
  public:
    explicit agent_t(EventLoop loop, std::optional<int> cpu_id = std::nullopt) noexcept
      : loop_(std::move(loop)), cpu_id_(cpu_id) {}

    agent_t(const agent_t&) = delete;
    agent_t& operator=(const agent_t&) = delete;
    agent_t(agent_t&&) noexcept = default;
    agent_t& operator=(agent_t&&) noexcept = default;

    /// Launch the worker thread. The system's stop request reaches the loop through the
    /// stop_token captured by the loop at construction; we do not need to forward it here.
    void start() {
      thread_ = std::jthread([this](std::stop_token /*own*/) {
        if (cpu_id_.has_value()) {
          (void) detail::pin_current_thread(*cpu_id_);
        }
        loop_.run();
      });
    }

    /// Block until the worker exits. Idempotent on a never-started or already-joined agent.
    void join() {
      if (thread_.joinable()) {
        thread_.join();
      }
    }

  private:
    EventLoop loop_;
    std::optional<int> cpu_id_{};
    std::jthread thread_{};
  };

  /// CTAD helper.
  template <typename EventLoop>
  agent_t(EventLoop, std::optional<int>) -> agent_t<EventLoop>;

  template <typename EventLoop>
  [[nodiscard]] inline auto make_agent(EventLoop&& loop, std::optional<int> cpu_id = std::nullopt) {
    return agent_t<std::decay_t<EventLoop>>(std::forward<EventLoop>(loop), cpu_id);
  }

}  // namespace matching::runtime
