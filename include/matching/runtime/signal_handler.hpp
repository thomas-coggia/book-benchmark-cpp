#pragma once

#include <csignal>
#include <stop_token>
#include <utility>

namespace matching::runtime {

  namespace detail {
    extern "C" void request_stop_signal_handler(int signum) noexcept;
  }

  /// RAII wrapper around @c std::signal: saves the previous handler, installs @p handler for
  /// @p signum, and restores it on destruction. Do not keep two active guards for the same
  /// @p signum unless lifetimes are deliberately nested; the second install overwrites the
  /// first, and destroying them out of order restores the wrong handler.
  class signal_guard {
  public:
    signal_guard() noexcept = default;

    explicit signal_guard(int signum, void (*handler)(int) noexcept) noexcept {
      void (*prev)(int) = std::signal(signum, handler);
      if (prev == SIG_ERR) {
        return;
      }
      signum_ = signum;
      previous_ = prev;
      active_ = true;
    }

    signal_guard(const signal_guard&) = delete;
    signal_guard& operator=(const signal_guard&) = delete;

    signal_guard(signal_guard&& other) noexcept {
      *this = std::move(other);
    }

    signal_guard& operator=(signal_guard&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      release();
      signum_ = other.signum_;
      previous_ = other.previous_;
      active_ = other.active_;
      other.active_ = false;
      other.signum_ = -1;
      return *this;
    }

    ~signal_guard() noexcept {
      release();
    }

    [[nodiscard]] bool is_active() const noexcept {
      return active_;
    }

  private:
    void release() noexcept {
      if (!active_) {
        return;
      }
      (void) std::signal(signum_, previous_);
      active_ = false;
      signum_ = -1;
    }

    int signum_{-1};
    void (*previous_)(int) = SIG_DFL;
    bool active_{false};
  };

  /// The single, process-global @c std::stop_source the signal handler flips on
  /// @c SIGINT/@c SIGTERM. Call sites pass a copy (or use @c get_token()) into the runtime;
  /// copies share the same stop state.
  ///
  /// Storage lives in @c src/signal_handler.cpp.
  [[nodiscard]] std::stop_source& global_stop_source() noexcept;

  /// Installs @c SIGINT and @c SIGTERM handlers that @c request_stop() on @ref global_stop_source.
  /// Restores the previous handlers when the returned object is destroyed.
  [[nodiscard]] inline auto make_stop_source_signal_guards() noexcept {
    struct guards {
      signal_guard sigint;
      signal_guard sigterm;
    };
    return guards{
      signal_guard{SIGINT, &detail::request_stop_signal_handler},
      signal_guard{SIGTERM, &detail::request_stop_signal_handler},
    };
  }

}  // namespace matching::runtime
