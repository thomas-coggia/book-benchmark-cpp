#pragma once

#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace matching::runtime {

  /// CPU-friendly idle hint for a busy-spin loop. On x86 emits @c PAUSE, which throttles the
  /// pipeline and avoids the memory-order-violation penalty on contended atomics; elsewhere
  /// yields the thread to the scheduler so we do not burn a full core polling.
  inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
  }

  /// Generic poll loop that drives a @c Source by handing every popped event to a @c Handler.
  ///
  /// Termination policy mirrors the runtime's two-mode shutdown contract:
  ///   - **Graceful** — the producer pushes @ref matching::shutdown_t through the data plane;
  ///     the handler observes it and returns @c true to break the loop. This guarantees that
  ///     every event already in the queue is drained.
  ///   - **Abort** — @c stop_token.stop_requested() flips (typically from a signal handler).
  ///     The loop exits before the next pop, possibly leaving in-flight events in the queue.
  ///
  /// @tparam Source  Concept: @c try_pop(T&) -> bool with a @c value_type alias.
  /// @tparam Handler Concept: @c (event) -> bool, returns @c true to request termination
  ///                 (i.e. the handler observed the in-band sentinel and finished its tail
  ///                 work). A @c void-returning handler is also accepted via the
  ///                 @ref drain_until template.
  template <typename Source, typename Handler>
  class event_loop_t {
  public:
    explicit event_loop_t(Source source, Handler handler, std::stop_token token) noexcept
      : source_(std::move(source)), handler_(std::move(handler)), token_(std::move(token)) {}

    /// Run until either the handler signals "done" (returns @c true on @ref matching::shutdown_t) or
    /// the stop token is requested.
    void run() {
      typename Source::value_type event{};
      while (!token_.stop_requested()) {
        if (source_.try_pop(event)) {
          if (handler_(event)) {
            return;
          }
        } else {
          cpu_pause();
        }
      }
    }

    [[nodiscard]] const std::stop_token& stop_token() const noexcept {
      return token_;
    }

  private:
    Source source_;
    Handler handler_;
    std::stop_token token_;
  };

  /// CTAD helper.
  template <typename Source, typename Handler>
  event_loop_t(Source, Handler, std::stop_token) -> event_loop_t<Source, Handler>;

  template <typename Source, typename Handler>
  [[nodiscard]] inline auto make_event_loop(Source&& source, Handler&& handler, std::stop_token token) {
    return event_loop_t<std::decay_t<Source>, std::decay_t<Handler>>(
      std::forward<Source>(source), std::forward<Handler>(handler), std::move(token));
  }

}  // namespace matching::runtime
