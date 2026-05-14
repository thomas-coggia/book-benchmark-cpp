#pragma once

#include <cstdint>

#include "matching/benchmark/tsc_timer.hpp"

namespace matching::benchmark {

  /// Default benchmark timer: serialised TSC on x86 GCC/Clang, monotonic clock fallback
  /// elsewhere. @ref bench_mark / @ref bench_elapsed_ns bracket latency samples on the matcher;
  /// cycle deltas are also available via @ref elapsed_cycles when callers prefer integers only.
  struct tsc_benchmark_timer_t {
    using mark_type = std::uint64_t;
    using cycle_type = std::uint64_t;

    [[nodiscard]] static mark_type mark() noexcept {
      return bench_mark();
    }

    [[nodiscard]] static cycle_type elapsed_cycles(mark_type m) noexcept {
      return bench_elapsed_cycles(m);
    }

    [[nodiscard]] static double elapsed_ns(mark_type m) noexcept {
      return bench_elapsed_ns(m);
    }

    [[nodiscard]] static double cycles_to_ns(cycle_type c) noexcept {
      return bench_cycles_to_ns(c);
    }
  };

}  // namespace matching::benchmark
