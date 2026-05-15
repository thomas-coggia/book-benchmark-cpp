#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace matching::benchmark {

  /// Single shape descriptor for the synthetic-market generator.  Driven by the
  /// continuous-double-auction literature (Smith/Farmer/Gillemot/Krishnamurthy 2003,
  /// Cont/Stoikov/Talreja 2010, Bouchaud/Mézard/Potters 2002), pared down to the knobs that
  /// exercise the matching engine's hot paths in distinct mixes.
  ///
  /// The mid price evolves as a discrete-time geometric Brownian walk on log-price, snapped
  /// to the integer tick grid; sides are Bernoulli-biased; non-aggressive limit-add prices
  /// sit on a truncated exponential of distance-from-touch; quantities are log-normal,
  /// integer-clamped; the add-vs-cancel mix is Bernoulli per event.
  struct market_profile_t {
    std::string_view name{};

    std::uint64_t seed{};
    std::size_t num_orders{};

    double cancel_ratio{};      ///< P(event is Cancel | live set is non-empty).
    double aggressive_ratio{};  ///< P(an Add crosses the opposite touch).
    double buy_bias{};          ///< P(side = Buy); 0.5 is neutral.

    double mu{};     ///< Mid log-drift per step.
    double sigma{};  ///< Mid log-volatility per step.

    std::int32_t initial_mid{};  ///< Starting mid in ticks.
    std::int32_t tick_size{};    ///< Ticks are integers; price = ticks * tick_size.

    double place_decay{};  ///< Exponential decay parameter for distance-from-touch on non-aggressive adds.

    double qty_log_mean{};    ///< Log-normal underlying mean.
    double qty_log_stddev{};  ///< Log-normal underlying stddev.
    std::int32_t qty_min{};
    std::int32_t qty_max{};
  };

  /// Low cancel, low aggressive, low sigma. Builds a deep, slowly-growing book; few matches.
  /// Stresses the Add path and price-level insertion / discovery.
  inline constexpr market_profile_t profile_quiet_build{
    .name = "quiet",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.05,
    .aggressive_ratio = 0.02,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00005,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.50,
    .qty_log_mean = 2.0,
    .qty_log_stddev = 0.6,
    .qty_min = 1,
    .qty_max = 1'000,
  };

  /// Moderate cancel, moderate aggressive, moderate sigma. Healthy mix; lots of single-level
  /// matches. Exercises the Add-with-match path along with FIFO time priority.
  inline constexpr market_profile_t profile_active_match{
    .name = "active",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.20,
    .aggressive_ratio = 0.30,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.0005,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.40,
    .qty_log_mean = 2.5,
    .qty_log_stddev = 0.8,
    .qty_min = 1,
    .qty_max = 1'000,
  };

  /// High cancel, low aggressive. Stresses cancel-path lookup, intrusive unlink, and level
  /// removal as orders are continuously withdrawn.
  inline constexpr market_profile_t profile_cancel_heavy{
    .name = "cancel",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.55,
    .aggressive_ratio = 0.05,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.0001,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.45,
    .qty_log_mean = 2.0,
    .qty_log_stddev = 0.5,
    .qty_min = 1,
    .qty_max = 500,
  };

  /// High sigma, mid aggressive. Stale resting orders get swept; the matching loop walks
  /// across multiple price levels per Add as the mid moves around.
  inline constexpr market_profile_t profile_volatile{
    .name = "volatile",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.15,
    .aggressive_ratio = 0.25,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.005,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.30,
    .qty_log_mean = 2.5,
    .qty_log_stddev = 0.8,
    .qty_min = 1,
    .qty_max = 1'000,
  };

  /// Large quantity mean plus high aggressive ratio. Big aggressive orders consume several
  /// price levels per Add; stresses the inner match-against-level loop and the partial-fill
  /// bookkeeping (resting reductions, level drops as they empty).
  inline constexpr market_profile_t profile_sweep{
    .name = "sweep",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.10,
    .aggressive_ratio = 0.50,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.0008,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.35,
    .qty_log_mean = 5.0,
    .qty_log_stddev = 1.0,
    .qty_min = 1,
    .qty_max = 10'000,
  };

}  // namespace matching::benchmark
