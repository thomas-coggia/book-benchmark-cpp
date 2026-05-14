#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace matching::benchmark {

  /// Aggregated statistics over a population of latency samples.
  struct latency_stats_t {
    double mean_ns{0.0};
    double stddev_ns{0.0};
    double min_ns{0.0};
    double max_ns{0.0};
    double p10_ns{0.0};
    double p50_ns{0.0};
    double p95_ns{0.0};
    double p99_ns{0.0};
    double p99_9_ns{0.0};
    std::size_t sample_count{0};
  };

  namespace detail {

    /// NumPy-style linear-interpolation quantile on a sorted ascending vector.
    [[nodiscard]] inline double linear_quantile(const std::vector<double>& sorted, double q) noexcept {
      if (sorted.empty()) {
        return 0.0;
      }
      q = std::clamp(q, 0.0, 1.0);
      if (sorted.size() == 1) {
        return sorted.front();
      }
      const double pos = static_cast<double>(sorted.size() - 1) * q;
      const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
      const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
      const double frac = pos - static_cast<double>(lo);
      return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
    }

  }  // namespace detail

  /// Online latency accumulator.
  ///
  /// Welford's algorithm gives streaming mean / stddev / min / max in constant memory; on top
  /// of that we retain up to @c max_samples_v raw samples (Vitter's reservoir sampling above
  /// the cap) so percentiles come out of an actual sorted population rather than a binned
  /// approximation. The cap keeps memory bounded under multi-million-event runs.
  class latency_accumulator_t {
  public:
    static constexpr std::size_t max_samples_v = 1'000'000;

    explicit latency_accumulator_t(std::uint64_t reservoir_seed = 0x9E3779B97F4A7C15ULL)
      : rng_(reservoir_seed) {
      samples_.reserve(std::min<std::size_t>(max_samples_v, 1u << 16));
    }

    /// Incorporate one latency reading.
    void add(double value_ns) {
      ++count_;
      const double delta = value_ns - mean_;
      mean_ += delta / static_cast<double>(count_);
      const double delta2 = value_ns - mean_;
      m2_ += delta * delta2;
      min_ = std::min(min_, value_ns);
      max_ = std::max(max_, value_ns);

      if (samples_.size() < max_samples_v) {
        samples_.push_back(value_ns);
      } else {
        // Reservoir sampling keeps every retained slot equally likely to be replaced
        // by the newest observation, so the distribution of stored samples mirrors
        // the full population.
        std::uniform_int_distribution<std::size_t> dist(1, count_);
        const std::size_t j = dist(rng_);
        if (j <= max_samples_v) {
          samples_[j - 1] = value_ns;
        }
      }
    }

    [[nodiscard]] latency_stats_t snapshot() const {
      latency_stats_t s{};
      s.sample_count = count_;
      if (count_ == 0) {
        return s;
      }
      s.mean_ns = mean_;
      s.stddev_ns = (count_ > 1) ? std::sqrt(m2_ / static_cast<double>(count_ - 1)) : 0.0;
      s.min_ns = (min_ == std::numeric_limits<double>::max()) ? 0.0 : min_;
      s.max_ns = (max_ == std::numeric_limits<double>::lowest()) ? 0.0 : max_;
      if (samples_.empty()) {
        return s;
      }
      std::vector<double> sorted = samples_;
      std::sort(sorted.begin(), sorted.end());
      s.p10_ns = detail::linear_quantile(sorted, 0.10);
      s.p50_ns = detail::linear_quantile(sorted, 0.50);
      s.p95_ns = detail::linear_quantile(sorted, 0.95);
      s.p99_ns = detail::linear_quantile(sorted, 0.99);
      s.p99_9_ns = detail::linear_quantile(sorted, 0.999);
      return s;
    }

    [[nodiscard]] std::size_t count() const noexcept {
      return count_;
    }

  private:
    std::size_t count_{0};
    double mean_{0.0};
    double m2_{0.0};
    double min_{std::numeric_limits<double>::max()};
    double max_{std::numeric_limits<double>::lowest()};
    std::vector<double> samples_{};
    std::mt19937_64 rng_;
  };

}  // namespace matching::benchmark
