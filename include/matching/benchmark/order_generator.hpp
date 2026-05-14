#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "matching/benchmark/market_profile.hpp"
#include "matching/input_event.hpp"

namespace matching::benchmark {

  /// Single generic generator parameterised by @ref market_profile_t. Produces a
  /// deterministic stream of @ref input_event_t — alternating Adds and Cancels in the
  /// configured proportions — with each call to @ref next.
  ///
  /// The mid price walks discretely as @c log(mid) += mu + sigma * N(0,1), then the
  /// integer mid is recovered by snapping to the tick grid. From the mid we derive a
  /// 2-tick spread and place each Add either:
  ///   * **aggressive** — at-or-through the opposite touch (will cross), or
  ///   * **passive**    — at the touch and then geometrically deeper, with intensity
  ///                      controlled by @c place_decay.
  /// Quantity is log-normal, integer-clamped. Live orderids are tracked in a vector so
  /// Cancels reference a real resting order with high probability.
  class order_generator_t {
  public:
    explicit order_generator_t(const market_profile_t& profile)
      : profile_(profile),
        rng_(profile.seed),
        log_mid_(std::log(static_cast<double>(profile.initial_mid))) {
      live_ids_.reserve(1u << 14);
    }

    /// Produce the next event. Cancels are issued only when the live set is non-empty;
    /// otherwise we fall through to an Add so the stream length always matches
    /// @c profile_.num_orders even on a starved cancel path.
    [[nodiscard]] input_event_t next() {
      step_mid();

      const bool want_cancel =
        !live_ids_.empty() && uniform_(rng_) < profile_.cancel_ratio;

      if (want_cancel) {
        const std::size_t i = std::uniform_int_distribution<std::size_t>{0, live_ids_.size() - 1}(rng_);
        const order_id_t id = live_ids_[i];
        // Swap-and-pop: O(1) removal that does not preserve insertion order, which is
        // irrelevant since cancels are uniform on the live set.
        live_ids_[i] = live_ids_.back();
        live_ids_.pop_back();
        return cancel_order_request_t{id};
      }

      const order_id_t id = ++last_order_id_;
      const side_t side = (uniform_(rng_) < profile_.buy_bias) ? side_t::buy : side_t::sell;
      const quantity_t quantity = sample_quantity();
      const price_t price = sample_price(side);

      live_ids_.push_back(id);
      return add_order_request_t{id, side, quantity, price};
    }

    /// Number of Add events generated whose ids are still in the live set (i.e. not yet
    /// cancelled by the generator). Used by the sanity tests.
    [[nodiscard]] std::size_t live_count() const noexcept {
      return live_ids_.size();
    }

    [[nodiscard]] order_id_t last_order_id() const noexcept {
      return last_order_id_;
    }

    [[nodiscard]] const market_profile_t& profile() const noexcept {
      return profile_;
    }

  private:
    void step_mid() noexcept {
      log_mid_ += profile_.mu + profile_.sigma * normal_(rng_);
    }

    [[nodiscard]] price_t snap_to_tick(double raw_price) const noexcept {
      const std::int32_t tick = std::max(profile_.tick_size, std::int32_t{1});
      // Guard against pathological mid drifts: a price below one tick is invalid for the
      // engine (positive integer prices only).
      auto rounded = static_cast<std::int64_t>(std::lround(raw_price));
      if (rounded < tick) {
        rounded = tick;
      }
      const std::int64_t snapped = (rounded / tick) * tick;
      return static_cast<price_t>(snapped == 0 ? tick : snapped);
    }

    [[nodiscard]] price_t sample_price(side_t side) {
      const double mid = std::exp(log_mid_);
      // 2-tick spread keeps the touch one tick either side of the mid.
      const std::int32_t half_spread = std::max(profile_.tick_size, std::int32_t{1});
      const double best_bid = mid - static_cast<double>(half_spread);
      const double best_ask = mid + static_cast<double>(half_spread);

      const bool aggressive = uniform_(rng_) < profile_.aggressive_ratio;
      if (aggressive) {
        // Aggressive: cross the opposite touch by 1..N ticks (geometric tail).
        const double depth_ticks = std::ceil(geometric_tail_ticks());
        if (side == side_t::buy) {
          return snap_to_tick(best_ask + depth_ticks * static_cast<double>(profile_.tick_size));
        }
        return snap_to_tick(best_bid - depth_ticks * static_cast<double>(profile_.tick_size));
      }

      // Passive: at the touch, then deeper with exponential intensity decay.
      const double depth_ticks = geometric_tail_ticks();
      if (side == side_t::buy) {
        return snap_to_tick(best_bid - depth_ticks * static_cast<double>(profile_.tick_size));
      }
      return snap_to_tick(best_ask + depth_ticks * static_cast<double>(profile_.tick_size));
    }

    /// Truncated exponential in @c [0, ∞), parameterised by @c place_decay. Larger
    /// @c place_decay → tail is shorter, prices cluster near the touch.
    [[nodiscard]] double geometric_tail_ticks() {
      const double lambda = std::max(profile_.place_decay, 1e-3);
      const double u = std::clamp(uniform_(rng_), 1e-9, 1.0 - 1e-9);
      return -std::log(u) / lambda;
    }

    [[nodiscard]] quantity_t sample_quantity() {
      const double log_q = profile_.qty_log_mean + profile_.qty_log_stddev * normal_(rng_);
      const double q = std::exp(log_q);
      const auto clamped = static_cast<std::int64_t>(std::clamp(
        std::lround(q),
        static_cast<long>(profile_.qty_min),
        static_cast<long>(profile_.qty_max)
      ));
      return static_cast<quantity_t>(std::max<std::int64_t>(clamped, 1));
    }

    market_profile_t profile_{};
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::normal_distribution<double> normal_{0.0, 1.0};

    double log_mid_{};
    order_id_t last_order_id_{0};
    std::vector<order_id_t> live_ids_{};
  };

}  // namespace matching::benchmark
