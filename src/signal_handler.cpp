#include "matching/runtime/signal_handler.hpp"

#include <atomic>
#include <csignal>
#include <stop_token>

namespace matching::runtime {

  namespace {

    /// The single canonical stop_source for the process. Function-local @c static so the
    /// initialisation order is deterministic and we avoid global ctor ordering pitfalls.
    [[nodiscard]] std::stop_source& canonical_stop_source() noexcept {
      static std::stop_source instance{};
      return instance;
    }

    /// Saved previous handlers, restored by @ref uninstall_signal_handler. Only the C
    /// library handler pointer needs preserving; @c std::signal is sufficient for our
    /// purposes (the operations we perform inside the handler — calling @c request_stop on
    /// a @c std::stop_source — are de-facto safe under glibc / libc++ / libstdc++ on
    /// Linux, just as in the reference architecture).
    std::atomic<bool> g_installed{false};
    void (*g_prev_sigint)(int) = SIG_DFL;
    void (*g_prev_sigterm)(int) = SIG_DFL;

    extern "C" void on_signal(int /*sig*/) noexcept {
      canonical_stop_source().request_stop();
    }

  }  // namespace

  std::stop_source& global_stop_source() noexcept {
    return canonical_stop_source();
  }

  void install_signal_handler() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return;  // Already installed; calling twice is a no-op.
    }
    g_prev_sigint = std::signal(SIGINT, &on_signal);
    g_prev_sigterm = std::signal(SIGTERM, &on_signal);
  }

  void uninstall_signal_handler() noexcept {
    bool expected = true;
    if (!g_installed.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
      return;  // Not installed.
    }
    (void) std::signal(SIGINT, g_prev_sigint);
    (void) std::signal(SIGTERM, g_prev_sigterm);
  }

}  // namespace matching::runtime
