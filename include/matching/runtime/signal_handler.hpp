#pragma once

#include <stop_token>

namespace matching::runtime {

  /// The single, process-global @c std::stop_source the signal handler flips on
  /// @c SIGINT/@c SIGTERM. Lives for the entire process lifetime; the binary's
  /// @c agent_system_t is built on top of @c global_stop_source().get_token() so a
  /// single signal cleanly aborts every agent.
  ///
  /// Implementation detail: the storage lives in @c src/signal_handler.cpp so there is a
  /// single instance across all translation units. The reference returned here is bound to
  /// that same object.
  [[nodiscard]] std::stop_source& global_stop_source() noexcept;

  /// Install @c SIGINT and @c SIGTERM handlers that call @c request_stop() on the global
  /// stop source. Safe to call exactly once at program startup; calling it twice is a no-op
  /// after the first invocation. Previous handlers are saved and restored by
  /// @ref uninstall_signal_handler.
  void install_signal_handler();

  /// Restore the signal handlers that were installed before @ref install_signal_handler.
  /// Idempotent: a no-op if @ref install_signal_handler was never called or the handlers
  /// have already been restored.
  void uninstall_signal_handler() noexcept;

}  // namespace matching::runtime
