#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "matching/benchmark/market_profile.hpp"
#include "matching/input_event.hpp"

namespace matching::benchmark {

  /// Generator parameterised by @ref market_profile_t. Each @ref next yields Add or Cancel in
  /// proportions driven by the profile (with optional hot/calm scaling). Cancels prefer
  /// near-touch liquidity when @c cancel_depth_lambda > 0. Adds use side autocorrelation when
  /// @c side_autocorr >= 0.
  class order_generator_t {
  public:
    explicit order_generator_t(const market_profile_t& profile)
      : profile_(profile), rng_(profile.seed), log_mid_(std::log(static_cast<double>(profile.initial_mid))) {
      live_.reserve(1u << 14);
    }

    [[nodiscard]] input_event_t next() {
      step_regime_markov();

      const double sigma_eff = profile_.sigma * (regime_hot_ ? profile_.hot_sigma_mul : 1.0);
      log_mid_ += profile_.mu + sigma_eff * normal_(rng_);

      const double cancel_p =
        std::clamp(profile_.cancel_ratio * (regime_hot_ ? profile_.hot_cancel_mul : 1.0), 0.0, 0.999);
      const double aggressive_p =
        std::clamp(profile_.aggressive_ratio * (regime_hot_ ? profile_.hot_aggressive_mul : 1.0), 0.0, 1.0);

      const bool want_cancel = !live_.empty() && uniform_(rng_) < cancel_p;

      if (want_cancel) {
        const price_t best_bid = current_best_bid();
        const price_t best_ask = current_best_ask();
        const std::size_t i = sample_cancel_index(best_bid, best_ask);
        const order_id_t id = live_[i].id;
        live_[i] = live_.back();
        live_.pop_back();
        return cancel_order_event_t{id};
      }

      const order_id_t id = ++last_order_id_;
      const side_t side = sample_side();
      const quantity_t quantity = sample_quantity();
      const price_t price = sample_price(side, aggressive_p);

      last_add_side_ = side;
      has_last_add_side_ = true;
      live_.push_back(live_resting_t{id, side, price});
      return add_order_event_t{id, side, quantity, price};
    }

    [[nodiscard]] std::size_t live_count() const noexcept {
      return live_.size();
    }

    [[nodiscard]] order_id_t last_order_id() const noexcept {
      return last_order_id_;
    }

    [[nodiscard]] const market_profile_t& profile() const noexcept {
      return profile_;
    }

  private:
    struct live_resting_t {
      order_id_t id{};
      side_t side{};
      price_t price{};
    };

    void step_regime_markov() noexcept {
      if (regime_hot_) {
        if (uniform_(rng_) < profile_.markov_leave_hot_prob) {
          regime_hot_ = false;
        }
      } else if (uniform_(rng_) < profile_.markov_enter_hot_prob) {
        regime_hot_ = true;
      }
    }

    [[nodiscard]] price_t snap_to_tick(double raw_price) const noexcept {
      const std::int32_t tick = std::max(profile_.tick_size, std::int32_t{1});
      auto rounded = static_cast<std::int64_t>(std::lround(raw_price));
      if (rounded < tick) {
        rounded = tick;
      }
      const std::int64_t snapped = (rounded / tick) * tick;
      return static_cast<price_t>(snapped == 0 ? tick : snapped);
    }

    [[nodiscard]] price_t current_best_bid() const noexcept {
      const double mid = std::exp(log_mid_);
      const std::int32_t half_spread = std::max(profile_.tick_size, std::int32_t{1});
      const double best_bid = mid - static_cast<double>(half_spread);
      return snap_to_tick(best_bid);
    }

    [[nodiscard]] price_t current_best_ask() const noexcept {
      const double mid = std::exp(log_mid_);
      const std::int32_t half_spread = std::max(profile_.tick_size, std::int32_t{1});
      const double best_ask = mid + static_cast<double>(half_spread);
      return snap_to_tick(best_ask);
    }

    [[nodiscard]] side_t sample_side() noexcept {
      if (profile_.side_autocorr < 0.0 || !has_last_add_side_) {
        return uniform_(rng_) < profile_.buy_bias ? side_t::buy : side_t::sell;
      }
      if (uniform_(rng_) < profile_.side_autocorr) {
        return last_add_side_;
      }
      return uniform_(rng_) < profile_.buy_bias ? side_t::buy : side_t::sell;
    }

    [[nodiscard]] std::size_t sample_cancel_index(price_t best_bid, price_t best_ask) noexcept {
      if (live_.size() == 1u || profile_.cancel_depth_lambda <= 0.0) {
        return std::uniform_int_distribution<std::size_t>{0, live_.size() - 1}(rng_);
      }
      const std::int32_t tick = std::max(profile_.tick_size, std::int32_t{1});
      const double lam = profile_.cancel_depth_lambda;
      double sum_w = 0.0;
      cancel_weights_.clear();
      cancel_weights_.reserve(live_.size());
      for (const auto& o : live_) {
        std::int64_t depth_ticks = 0;
        if (o.side == side_t::buy) {
          depth_ticks = std::max<std::int64_t>(
            0, (static_cast<std::int64_t>(best_bid) - static_cast<std::int64_t>(o.price)) / tick
          );
        } else {
          depth_ticks = std::max<std::int64_t>(
            0, (static_cast<std::int64_t>(o.price) - static_cast<std::int64_t>(best_ask)) / tick
          );
        }
        const double w = std::exp(-lam * static_cast<double>(depth_ticks));
        cancel_weights_.push_back(w);
        sum_w += w;
      }
      if (sum_w <= 0.0) {
        return std::uniform_int_distribution<std::size_t>{0, live_.size() - 1}(rng_);
      }
      double u = uniform_(rng_) * sum_w;
      for (std::size_t i = 0; i < cancel_weights_.size(); ++i) {
        u -= cancel_weights_[i];
        if (u <= 0.0) {
          return i;
        }
      }
      return cancel_weights_.size() - 1;
    }

    [[nodiscard]] price_t sample_price(side_t side, double aggressive_prob) noexcept {
      const double mid = std::exp(log_mid_);
      const std::int32_t half_spread = std::max(profile_.tick_size, std::int32_t{1});
      const double best_bid = mid - static_cast<double>(half_spread);
      const double best_ask = mid + static_cast<double>(half_spread);

      const bool aggressive = uniform_(rng_) < aggressive_prob;
      if (aggressive) {
        const double depth_ticks = std::ceil(geometric_tail_ticks());
        if (side == side_t::buy) {
          return snap_to_tick(best_ask + depth_ticks * static_cast<double>(profile_.tick_size));
        }
        return snap_to_tick(best_bid - depth_ticks * static_cast<double>(profile_.tick_size));
      }

      const double depth_ticks = geometric_tail_ticks();
      if (side == side_t::buy) {
        return snap_to_tick(best_bid - depth_ticks * static_cast<double>(profile_.tick_size));
      }
      return snap_to_tick(best_ask + depth_ticks * static_cast<double>(profile_.tick_size));
    }

    [[nodiscard]] double geometric_tail_ticks() noexcept {
      const double lambda = std::max(profile_.place_decay, 1e-3);
      const double u = std::clamp(uniform_(rng_), 1e-9, 1.0 - 1e-9);
      return -std::log(u) / lambda;
    }

    [[nodiscard]] quantity_t sample_quantity() noexcept {
      const double log_q = profile_.qty_log_mean + profile_.qty_log_stddev * normal_(rng_);
      const double q = std::exp(log_q);
      const auto clamped = static_cast<std::int64_t>(
        std::clamp(std::lround(q), static_cast<long>(profile_.qty_min), static_cast<long>(profile_.qty_max))
      );
      return static_cast<quantity_t>(std::max<std::int64_t>(clamped, 1));
    }

    market_profile_t profile_{};
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::normal_distribution<double> normal_{0.0, 1.0};

    double log_mid_{};
    order_id_t last_order_id_{0};
    std::vector<live_resting_t> live_{};

    bool regime_hot_{false};
    bool has_last_add_side_{false};
    side_t last_add_side_{};
    std::vector<double> cancel_weights_{};
  };

}  // namespace matching::benchmark
