#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace matching::benchmark {

  /// Descriptor for the synthetic order stream used by @ref order_generator_t and the benchmark
  /// harness. Knobs borrow loosely from continuous-double-auction and LOB literature (e.g. Smith
  /// et al. 2003; Cont–Stoikov–Talreja 2010), reduced to a small set meant to stress distinct
  /// matcher code paths. This is not a calibrated venue model.
  ///
  /// The log-mid follows a discrete geometric random walk, snapped to the tick grid. Each step
  /// emits exactly one Add or one Cancel (Cancel requires a resting order id). The scalar
  /// @c cancel_ratio is **not** the unconditional share of Cancel messages: it is @e P(Cancel |
  /// live book non-empty). Unconditional Add/Cancel proportions depend on starvation when the
  /// book is empty and on overlays (continuous latent heat and spread-conditioned aggression).
  struct market_profile_t {
    std::string_view name{};

    std::uint64_t seed{};
    std::size_t num_orders{};

    double cancel_ratio{};      ///< Baseline P(cancel | live book non-empty), calm regime.
    double aggressive_ratio{};  ///< Baseline P(Add crosses opposite touch), calm regime.
    ///< Aggressive-add probability scale @f$\exp(-k\,s)@f$ after heat; @f$s@f$ = bid+ask half-width in
    ///< ticks. @f$k=0@f$ disables spread feedback.
    double aggression_spread_k{0.0};
    double buy_bias{};  ///< P(Buy) when drawing an uncorrelated side.

    double mu{};
    double sigma{};

    std::int32_t initial_mid{};
    std::int32_t tick_size{};

    double place_decay{};

    double qty_log_mean{};
    double qty_log_stddev{};
    std::int32_t qty_min{};
    std::int32_t qty_max{};

    ///< Side persistence on successive Adds when non-negative; IID Bernoulli(@c buy_bias) when negative.
    double side_autocorr{-1.0};

    ///< Cancel sampling weights @e exp(-lambda * depth_ticks) toward the touch; zero disables weighting.
    double cancel_depth_lambda{0.0};

    ///< Mean-reverting latent heat @f$h\in[0,1]@f$: OU-style @f$h\leftarrow h+\kappa(\bar h-h)+\sigma_h\xi@f$ each
    ///< primary step (then clamped).
    double heat_mean{0.25};
    ///< Mean-reversion strength @f$\kappa@f$ per primary step (non-negative).
    double heat_kappa{0.04};
    ///< Gaussian innovation scale @f$\sigma_h@f$ per primary step (non-negative).
    double heat_sigma{0.06};

    ///< Multipliers at @f$h=1@f$; at @f$h=0@f$ baseline applies. Effective
    ///< @f$m_{\mathrm{eff}}=1+h(m_{\mathrm{hot}}-1)@f$.
    double hot_cancel_mul{1.0};
    double hot_aggressive_mul{1.0};
    double hot_sigma_mul{1.0};

    ///< Extra half-spread ticks blended by @f$h@f$ (rounded):
    ///< @f$\mathrm{round}(h\cdot\mathrm{ticks}_{\mathrm{extra}})@f$.
    std::int32_t hot_half_spread_extra_ticks{0};
    ///< Nominal bid/ask half-distance from mid in ticks (each side), lower-bounded by one tick.
    std::int32_t base_half_spread_ticks{1};
    ///< Bid half-spread increases by this many ticks; ask decreases by the same (each clamped).
    std::int32_t spread_asym_ticks{0};
  };

  /// Deep passive liquidity; low toxicity and near-touch interaction.
  inline constexpr market_profile_t profile_quiet_build{
    .name = "quiet",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.84,
    .aggressive_ratio = 0.018,
    .aggression_spread_k = 0.045,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00004,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.62,
    .qty_log_mean = 2.0,
    .qty_log_stddev = 0.6,
    .qty_min = 1,
    .qty_max = 1'000,
    .side_autocorr = 0.60,
    .cancel_depth_lambda = 1.8,
    .heat_mean = 0.18,
    .heat_kappa = 0.038,
    .heat_sigma = 0.025,
    .hot_cancel_mul = 1.12,
    .hot_aggressive_mul = 1.45,
    .hot_sigma_mul = 2.8,
    .hot_half_spread_extra_ticks = 1,
    .base_half_spread_ticks = 1,
    .spread_asym_ticks = 0,
  };

  /// Tight liquid tape: competitive quotes and crossing without extreme diffusion (healthy liquidity regimes).
  inline constexpr market_profile_t profile_active_match{
    .name = "active",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.80,
    .aggressive_ratio = 0.18,
    .aggression_spread_k = 0.076,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00018,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.55,
    .qty_log_mean = 2.5,
    .qty_log_stddev = 0.8,
    .qty_min = 1,
    .qty_max = 1'000,
    .side_autocorr = 0.64,
    .cancel_depth_lambda = 2.0,
    .heat_mean = 0.34,
    .heat_kappa = 0.060,
    .heat_sigma = 0.058,
    .hot_cancel_mul = 1.08,
    .hot_aggressive_mul = 1.38,
    .hot_sigma_mul = 2.2,
    .hot_half_spread_extra_ticks = 1,
    .base_half_spread_ticks = 1,
    .spread_asym_ticks = 0,
  };

  /// Cancel-dominated stream; stresses allocator and intrusive list tear-down.
  inline constexpr market_profile_t profile_cancel_heavy{
    .name = "cancel",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.95,
    .aggressive_ratio = 0.012,
    .aggression_spread_k = 0.095,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00009,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.55,
    .qty_log_mean = 2.0,
    .qty_log_stddev = 0.5,
    .qty_min = 1,
    .qty_max = 500,
    .side_autocorr = 0.52,
    .cancel_depth_lambda = 2.4,
    .heat_mean = 0.12,
    .heat_kappa = 0.032,
    .heat_sigma = 0.042,
    .hot_cancel_mul = 1.05,
    .hot_aggressive_mul = 1.25,
    .hot_sigma_mul = 1.9,
    .hot_half_spread_extra_ticks = 1,
    .base_half_spread_ticks = 1,
    .spread_asym_ticks = 0,
  };

  /// Stress from asymmetric liquidity, spread widening, and directional persistence rather than pure mid diffusion.
  inline constexpr market_profile_t profile_volatile{
    .name = "volatile",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.78,
    .aggressive_ratio = 0.24,
    .aggression_spread_k = 0.055,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.0020,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.25,
    .qty_log_mean = 2.5,
    .qty_log_stddev = 0.8,
    .qty_min = 1,
    .qty_max = 1'000,
    .side_autocorr = 0.74,
    .cancel_depth_lambda = 1.85,
    .heat_mean = 0.55,
    .heat_kappa = 0.026,
    .heat_sigma = 0.14,
    .hot_cancel_mul = 1.22,
    .hot_aggressive_mul = 1.55,
    .hot_sigma_mul = 2.0,
    .hot_half_spread_extra_ticks = 3,
    .base_half_spread_ticks = 1,
    .spread_asym_ticks = 2,
  };

  /// Directional execution stress: toxic flow, depleted queues, wide asymmetric quotes.
  inline constexpr market_profile_t profile_sweep{
    .name = "sweep",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.62,
    .aggressive_ratio = 0.72,
    .aggression_spread_k = 0.0,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.0012,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.20,
    .qty_log_mean = 5.0,
    .qty_log_stddev = 1.0,
    .qty_min = 1,
    .qty_max = 10'000,
    .side_autocorr = 0.85,
    .cancel_depth_lambda = 0.9,
    .heat_mean = 0.60,
    .heat_kappa = 0.038,
    .heat_sigma = 0.068,
    .hot_cancel_mul = 1.18,
    .hot_aggressive_mul = 1.28,
    .hot_sigma_mul = 2.0,
    .hot_half_spread_extra_ticks = 3,
    .base_half_spread_ticks = 1,
    .spread_asym_ticks = 2,
  };

}  // namespace matching::benchmark
