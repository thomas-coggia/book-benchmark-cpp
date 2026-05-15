#include "matching/runtime/signal_handler.hpp"

namespace matching::runtime {

  std::stop_source& global_stop_source() noexcept {
    /// Function-local @c static: one object for the process, deterministic init on first use,
    /// no global ctor ordering issues.
    static std::stop_source instance{};
    return instance;
  }

  namespace detail {

    extern "C" void request_stop_signal_handler(int /*signum*/) noexcept {
      global_stop_source().request_stop();
    }

  }  // namespace detail

}  // namespace matching::runtime
