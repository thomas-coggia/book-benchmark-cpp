#pragma once

#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace matching::runtime {

  /// Busy-spin throttle (PAUSE on x86; yield elsewhere).
  inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
  }

  /// Polls source.try_pop into handler until handler returns true (graceful sentinel) or stop_token stops (abort).
  ///
  /// Source: try_pop(T&) -> bool, value_type. Handler: (event) -> bool (true = shut down).
  template <typename Source, typename Handler>
  class event_loop_t {
  public:
    explicit event_loop_t(Source source, Handler handler, std::stop_token token) noexcept
      : source_(std::move(source)), handler_(std::move(handler)), token_(std::move(token)) {}

    /// Runs until handler returns true or stop is requested.
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

  template <typename Source, typename Handler>
  event_loop_t(Source, Handler, std::stop_token) -> event_loop_t<Source, Handler>;

  template <typename Source, typename Handler>
  [[nodiscard]] inline auto make_event_loop(Source&& source, Handler&& handler, std::stop_token token) {
    return event_loop_t<std::decay_t<Source>, std::decay_t<Handler>>(
      std::forward<Source>(source), std::forward<Handler>(handler), std::move(token)
    );
  }

}  // namespace matching::runtime
