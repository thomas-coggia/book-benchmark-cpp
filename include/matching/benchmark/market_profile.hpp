#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace matching::benchmark {

  /// Single shape descriptor for the synthetic-market generator. Knobs are chosen to
  /// separate matching-engine stress modes while leaning toward **message composition**
  /// closer to electronic markets: dominant cancel traffic, short-lived regime shifts,
  /// distance-aware withdrawal of liquidity, and buy/sell persistence (see Cont/Kukanov
  /// 2013-style order-flow autocorrelation). This remains a **toy** simulator — no
  /// replaces, no strategic queue games — but presets aim for plausible **add vs cancel**
  /// mixes rather than execution-heavy IID streams.
  ///
  /// Mid price evolves as a discrete-time geometric Brownian walk on log-price, snapped
  /// to the integer tick grid. Non-aggressive limit-add prices sit on a truncated
  /// exponential of distance-from-touch; quantities are log-normal; each event is Add or
  /// Cancel with configurable odds (Cancel requires a live resting id).
  struct market_profile_t {
    std::string_view name{};

    std::uint64_t seed{};
    std::size_t num_orders{};

    double cancel_ratio{};      ///< Baseline P(event is Cancel | live set is non-empty).
    double aggressive_ratio{};  ///< Baseline P(an Add crosses the opposite touch).
    double buy_bias{};          ///< P(side = Buy) when drawing an uncorrelated side.

    double mu{};     ///< Mid log-drift per step.
    double sigma{};  ///< Mid log-volatility per step (scaled in hot regime).

    std::int32_t initial_mid{};
    std::int32_t tick_size{};

    double place_decay{};  ///< Passive depth decay (larger → tighter around touch).

    double qty_log_mean{};
    double qty_log_stddev{};
    std::int32_t qty_min{};
    std::int32_t qty_max{};

    /// When @c >= 0 , each Add repeats the previous side with this probability; otherwise
    /// (( @c < 0 )) sides are IID Bernoulli(@c buy_bias).
    double side_autocorr{-1.0};

    /// When @c > 0 , Cancels prefer liquidity closer to the touch (@e exp(-λ·depth)).
    double cancel_depth_lambda{0.0};

    /// Two-state ("calm" / "hot") Markov overlay: probability per event to enter hot from calm.
    double markov_enter_hot_prob{0.0};
    /// Probability per event to leave hot (ignored while calm).
    double markov_leave_hot_prob{0.003};

    double hot_cancel_mul{1.0};      ///< Multiplier on @c cancel_ratio while hot.
    double hot_aggressive_mul{1.0};  ///< Multiplier on @c aggressive_ratio while hot.
    double hot_sigma_mul{1.0};       ///< Multiplier on @c sigma while hot.
  };

  /// Calm **maker-style** depth building with heavy quote churn: high cancel share, tiny
  /// aggressive fraction, mild side persistence, occasional hot bursts (wider effective
  /// vol + more aggression). Targets low trades-per-add versus legacy presets.
  inline constexpr market_profile_t profile_quiet_build{
    .name = "quiet",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.78,
    .aggressive_ratio = 0.018,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00004,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.52,
    .qty_log_mean = 2.0,
    .qty_log_stddev = 0.6,
    .qty_min = 1,
    .qty_max = 1'000,
    .side_autocorr = 0.58,
    .cancel_depth_lambda = 1.6,
    .markov_enter_hot_prob = 0.00035,
    .markov_leave_hot_prob = 0.0045,
    .hot_cancel_mul = 1.12,
    .hot_aggressive_mul = 1.45,
    .hot_sigma_mul = 2.8,
  };

  /// Mixed continuous trading: very high cancel traffic (modern venues), moderate
  /// aggression, volatility coupled through hot regimes rather than a single static σ.
  inline constexpr market_profile_t profile_active_match{
    .name = "active",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.86,
    .aggressive_ratio = 0.26,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00042,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.42,
    .qty_log_mean = 2.5,
    .qty_log_stddev = 0.8,
    .qty_min = 1,
    .qty_max = 1'000,
    .side_autocorr = 0.62,
    .cancel_depth_lambda = 2.0,
    .markov_enter_hot_prob = 0.00055,
    .markov_leave_hot_prob = 0.0028,
    .hot_cancel_mul = 1.08,
    .hot_aggressive_mul = 1.38,
    .hot_sigma_mul = 2.2,
  };

  /// Cancel-heavy, low aggression: stresses unlink paths with **realistic** non-trade message
  /// dominance (often >90% cancels among adds+cancels).
  inline constexpr market_profile_t profile_cancel_heavy{
    .name = "cancel",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.92,
    .aggressive_ratio = 0.034,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00009,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.46,
    .qty_log_mean = 2.0,
    .qty_log_stddev = 0.5,
    .qty_min = 1,
    .qty_max = 500,
    .side_autocorr = 0.56,
    .cancel_depth_lambda = 2.6,
    .markov_enter_hot_prob = 0.00015,
    .markov_leave_hot_prob = 0.006,
    .hot_cancel_mul = 1.05,
    .hot_aggressive_mul = 1.25,
    .hot_sigma_mul = 1.9,
  };

  /// Volatility **coupled** to liquidity stress: frequent hot spells lift σ and pull ratios;
  /// elevated baseline cancel simulates retreating liquidity when the mid moves.
  inline constexpr market_profile_t profile_volatile{
    .name = "volatile",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.63,
    .aggressive_ratio = 0.15,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.0035,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.34,
    .qty_log_mean = 2.5,
    .qty_log_stddev = 0.8,
    .qty_min = 1,
    .qty_max = 1'000,
    .side_autocorr = 0.60,
    .cancel_depth_lambda = 1.85,
    .markov_enter_hot_prob = 0.0011,
    .markov_leave_hot_prob = 0.0016,
    .hot_cancel_mul = 1.22,
    .hot_aggressive_mul = 1.55,
    .hot_sigma_mul = 3.2,
  };

  /// Deliberately **non-stationary** microstructure stress: large sizes, persistent
  /// directional flow, moderate cancel share so depth actually forms, episodic hot regimes.
  /// Trade ratios stay elevated versus production — intended as a matcher torture test.
  inline constexpr market_profile_t profile_sweep{
    .name = "sweep",
    .seed = 42,
    .num_orders = 1'000'000,
    .cancel_ratio = 0.38,
    .aggressive_ratio = 0.52,
    .buy_bias = 0.50,
    .mu = 0.0,
    .sigma = 0.00075,
    .initial_mid = 10'000,
    .tick_size = 1,
    .place_decay = 0.38,
    .qty_log_mean = 5.0,
    .qty_log_stddev = 1.0,
    .qty_min = 1,
    .qty_max = 10'000,
    .side_autocorr = 0.68,
    .cancel_depth_lambda = 1.2,
    .markov_enter_hot_prob = 0.0009,
    .markov_leave_hot_prob = 0.002,
    .hot_cancel_mul = 1.18,
    .hot_aggressive_mul = 1.28,
    .hot_sigma_mul = 2.0,
  };

}  // namespace matching::benchmark
