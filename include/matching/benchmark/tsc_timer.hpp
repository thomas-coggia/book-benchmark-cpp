#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>

#if defined(__linux__)
#include <ctime>
#endif

#if (defined(__x86_64__) || defined(__i386__)) &&                              \
    (defined(__GNUC__) || defined(__clang__))
#include <x86intrin.h>
#define MATCHING_BENCHMARK_HAVE_X86_TSC 1
#else
#define MATCHING_BENCHMARK_HAVE_X86_TSC 0
#endif

namespace matching::benchmark {

/// Monotonic nanosecond clock used as the TSC calibration anchor and as the
/// fallback timer on non-x86 hosts. @c CLOCK_MONOTONIC_RAW avoids NTP frequency
/// adjustments so short-interval deltas remain stable.
[[nodiscard]] inline std::int64_t monotonic_now_ns() noexcept {
#if defined(__linux__)
  struct timespec ts {};
  if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL +
           static_cast<std::int64_t>(ts.tv_nsec);
  }
#endif
  const auto epoch = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count();
}

#if MATCHING_BENCHMARK_HAVE_X86_TSC

namespace detail {

/// Read @c /sys/devices/system/cpu/cpu0/tsc_freq_khz, which the kernel exposes
/// when the invariant TSC is reliable. Returns false when the file is absent or
/// unparseable so we transparently fall back to a wall-clock spin calibration.
[[nodiscard]] inline bool read_sysfs_tsc_khz(std::uint32_t &khz) noexcept {
  std::ifstream in("/sys/devices/system/cpu/cpu0/tsc_freq_khz");
  unsigned long v = 0;
  if (in >> v && v > 0) {
    khz = static_cast<std::uint32_t>(v);
    return true;
  }
  return false;
}

/// Solve for nanoseconds-per-cycle. Trust the kernel's @c tsc_freq_khz when
/// available; otherwise spin against @ref monotonic_now_ns for ~25 ms and
/// divide.
[[nodiscard]] inline double calibrate_ns_per_tsc_cycle() noexcept {
  std::uint32_t khz = 0;
  if (read_sysfs_tsc_khz(khz)) {
    return 1'000'000.0 / static_cast<double>(khz);
  }
  constexpr std::int64_t min_wall_ns = 25'000'000;
  const std::uint64_t t0 = static_cast<std::uint64_t>(__builtin_ia32_rdtsc());
  const std::int64_t w0 = monotonic_now_ns();
  std::uint64_t t1 = t0;
  std::int64_t w1 = w0;
  do {
    t1 = static_cast<std::uint64_t>(__builtin_ia32_rdtsc());
    w1 = monotonic_now_ns();
  } while ((w1 - w0) < min_wall_ns);
  const double wall_ns = static_cast<double>(w1 - w0);
  const double dcycles = static_cast<double>(t1 - t0);
  return wall_ns / dcycles;
}

[[nodiscard]] inline double &ns_per_tsc_cycle_storage() noexcept {
  static double v = 0.0;
  return v;
}

inline void init_once() noexcept {
  static std::once_flag flag;
  std::call_once(
      flag, [] { ns_per_tsc_cycle_storage() = calibrate_ns_per_tsc_cycle(); });
}

/// @c lfence + @c rdtsc is the conventional micro-benchmark recipe: the fence
/// prevents the counter read from being reordered above/below the measured
/// region.
[[nodiscard]] inline std::uint64_t tsc_read_serialized() noexcept {
  _mm_lfence();
  return static_cast<std::uint64_t>(__builtin_ia32_rdtsc());
}

} // namespace detail

/// Force calibration up-front so the very first measurement does not pay for
/// the spin.
inline void benchmark_timer_init() noexcept { detail::init_once(); }

/// Capture a timestamp suitable for differencing against @ref bench_elapsed_ns or
/// @ref bench_elapsed_cycles.
[[nodiscard]] inline std::uint64_t bench_mark() noexcept {
  detail::init_once();
  return detail::tsc_read_serialized();
}

/// Raw TSC cycles elapsed since the @c mark returned by @ref bench_mark. Integer-only delta;
/// multiply by @ref bench_ns_per_cycle or call @ref bench_elapsed_ns for nanoseconds.
[[nodiscard]] inline std::uint64_t bench_elapsed_cycles(std::uint64_t mark) noexcept {
  detail::init_once();
  const std::uint64_t end = detail::tsc_read_serialized();
  return end - mark;
}

/// Nanoseconds elapsed since the @c mark returned by @ref bench_mark. Convenience wrapper
/// for callers that do not separate timing from analysis (single-thread tests, etc).
[[nodiscard]] inline double bench_elapsed_ns(std::uint64_t mark) noexcept {
  return static_cast<double>(bench_elapsed_cycles(mark)) * detail::ns_per_tsc_cycle_storage();
}

/// Convert a previously-captured cycle delta to nanoseconds. Pure scalar multiply.
[[nodiscard]] inline double bench_cycles_to_ns(std::uint64_t cycles) noexcept {
  detail::init_once();
  return static_cast<double>(cycles) * detail::ns_per_tsc_cycle_storage();
}

[[nodiscard]] inline double bench_ns_per_cycle() noexcept {
  detail::init_once();
  return detail::ns_per_tsc_cycle_storage();
}

#else

inline void benchmark_timer_init() noexcept {}

[[nodiscard]] inline std::uint64_t bench_mark() noexcept {
  return static_cast<std::uint64_t>(monotonic_now_ns());
}

/// On the fallback timer "cycles" are nanoseconds; the storage type is the same.
[[nodiscard]] inline std::uint64_t bench_elapsed_cycles(std::uint64_t mark) noexcept {
  const std::int64_t now = monotonic_now_ns();
  return static_cast<std::uint64_t>(now - static_cast<std::int64_t>(mark));
}

[[nodiscard]] inline double bench_elapsed_ns(std::uint64_t mark) noexcept {
  return static_cast<double>(bench_elapsed_cycles(mark));
}

[[nodiscard]] inline double bench_cycles_to_ns(std::uint64_t cycles) noexcept {
  return static_cast<double>(cycles);
}

[[nodiscard]] inline double bench_ns_per_cycle() noexcept { return 0.0; }

#endif

} // namespace matching::benchmark
