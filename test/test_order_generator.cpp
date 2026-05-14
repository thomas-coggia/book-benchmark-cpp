#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <unordered_set>
#include <variant>
#include <vector>

#include "matching/benchmark/market_profile.hpp"
#include "matching/benchmark/order_generator.hpp"
#include "matching/input_event.hpp"
#include "matching/order_book.hpp"
#include "matching/output_event.hpp"

namespace matching::benchmark {

  // Discard sink: drives the engine without recording outputs, only counting trades.
  struct trade_counter_t {
    std::size_t* trades{nullptr};
    void operator()(const trade_event_t&) const noexcept {
      ++*trades;
    }
    void operator()(const order_fully_filled_t&) const noexcept {}
    void operator()(const order_partially_filled_t&) const noexcept {}
  };

  struct GeneratorSanityTest : ::testing::TestWithParam<market_profile_t> {};

  // Run a small batch through the engine and assert: orderids are strictly increasing,
  // cancels only target previously-added ids, the realised cancel rate is in a sensible
  // band around the configured one, and the engine never crashes.
  TEST_P(GeneratorSanityTest, SmallNDoesNotCrashAndRespectsConstraints) {
    market_profile_t p = GetParam();
    p.num_orders = 10'000;

    order_generator_t gen{p};

    std::size_t trades = 0;
    clob_t<trade_counter_t> book{1u << 17, trade_counter_t{&trades}};

    std::unordered_set<order_id_t> added_ids;
    std::size_t cancels = 0;
    std::size_t adds = 0;
    order_id_t prev_add_id = 0;

    for (std::size_t i = 0; i < p.num_orders; ++i) {
      const matching::input_event_t e = gen.next();
      std::visit([&](const auto& concrete) {
        using T = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<T, matching::add_order_request_t>) {
          ASSERT_GT(concrete.order_id, prev_add_id) << "Order ids must be strictly increasing";
          ASSERT_GT(concrete.quantity, 0);
          ASSERT_GT(concrete.price, 0);
          prev_add_id = concrete.order_id;
          added_ids.insert(concrete.order_id);
          ++adds;
        } else if constexpr (std::is_same_v<T, matching::cancel_order_request_t>) {
          ASSERT_NE(added_ids.find(concrete.order_id), added_ids.end())
            << "Cancel must reference a previously added id";
          ++cancels;
        } else {
          // The generator never emits shutdown_t; an arrival here would be a generator bug.
          // The branch exists so the variant visit covers every alternative.
          FAIL() << "Generator emitted an unexpected shutdown_t";
        }
      }, e);
      book(e);
    }

    EXPECT_GT(adds, 0u);

    if (p.cancel_ratio > 0.001 && p.cancel_ratio < 0.999) {
      const double realised = static_cast<double>(cancels) / static_cast<double>(p.num_orders);
      EXPECT_NEAR(realised, p.cancel_ratio, 0.10)
        << "Realised cancel rate " << realised << " too far from configured " << p.cancel_ratio;
    }

    // A non-trivial fraction of presets should produce trades. We check that for
    // trade-heavy presets (sweep, active, volatile) the engine actually matches.
    if (p.aggressive_ratio >= 0.20) {
      EXPECT_GT(trades, 0u) << "Aggressive-heavy preset produced no trades";
    }
  }

  INSTANTIATE_TEST_SUITE_P(
    Presets,
    GeneratorSanityTest,
    ::testing::Values(
      profile_quiet_build,
      profile_active_match,
      profile_cancel_heavy,
      profile_volatile,
      profile_sweep
    ),
    [](const ::testing::TestParamInfo<market_profile_t>& info) -> std::string {
      return std::string{info.param.name};
    }
  );

}  // namespace matching::benchmark
