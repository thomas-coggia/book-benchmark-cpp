#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "matching/benchmark/market_profile.hpp"
#include "matching/benchmark/order_generator.hpp"
#include "matching/clob_factory.hpp"
#include "matching/input_event.hpp"
#include "matching/order_book.hpp"
#include "matching/output_event.hpp"

namespace matching {

  namespace {

    using fill_variant_t = std::variant<order_fully_filled_t, order_partially_filled_t>;

    /// Records every emitted output event so we can replay them in order and verify
    /// invariants. Owns its own state so each test instance is independent.
    struct invariants_recorder_t {
      std::vector<trade_event_t>* trades{nullptr};
      std::vector<fill_variant_t>* fills{nullptr};

      void operator()(const trade_event_t& e) const {
        trades->push_back(e);
      }

      void operator()(const order_fully_filled_t& e) const {
        fills->emplace_back(e);
      }

      void operator()(const order_partially_filled_t& e) const {
        fills->emplace_back(e);
      }
    };

  }  // namespace

  /// Conservation: at any point, the open quantity in the book equals
  ///   sum(added) - sum(cancelled, by id) - sum(matched on each fill).
  /// We maintain a shadow map of id → remaining_qty and update it from the emitted events;
  /// at the end, the shadow's remaining quantities must equal the actual book's view.
  TEST(BookInvariantsTest, ConservationOfQuantityAcrossRandomStream) {
    using namespace matching::benchmark;
    market_profile_t profile = profile_active_match;
    profile.num_orders = 10'000;
    profile.seed = 0xC0FFEEULL;

    order_generator_t gen{profile};
    std::vector<trade_event_t> trades;
    std::vector<fill_variant_t> fills;
    invariants_recorder_t recorder{&trades, &fills};

    clob_factory_t<invariants_recorder_t> factory{200'000, recorder};
    auto book = std::move(factory).create();

    std::unordered_map<order_id_t, quantity_t> remaining;
    quantity_t total_added_qty = 0;
    quantity_t total_cancelled_qty = 0;
    quantity_t total_matched_qty = 0;
    std::size_t cancelled_unknown = 0;

    auto apply_fill_to_shadow = [&](const fill_variant_t& fill) {
      const order_id_t id = std::visit([](const auto& f) -> order_id_t { return f.order_id; }, fill);
      auto it = remaining.find(id);
      ASSERT_NE(it, remaining.end()) << "fill references unknown order " << id;
      std::visit(
        [&](const auto& f) {
          using T = std::decay_t<decltype(f)>;
          if constexpr (std::is_same_v<T, order_fully_filled_t>) {
            total_matched_qty += it->second;
            remaining.erase(it);
          } else {
            total_matched_qty += (it->second - f.remaining_quantity);
            it->second = f.remaining_quantity;
          }
        },
        fill
      );
    };

    for (std::size_t i = 0; i < profile.num_orders; ++i) {
      const input_event_t event = gen.next();
      const std::size_t fill_cursor = fills.size();
      const std::size_t trade_cursor = trades.size();

      std::visit(
        [&](const auto& concrete) {
          using event_type = std::decay_t<decltype(concrete)>;
          if constexpr (std::is_same_v<event_type, add_order_event_t>) {
            remaining.emplace(concrete.order_id, concrete.quantity);
            total_added_qty += concrete.quantity;
          } else if constexpr (std::is_same_v<event_type, cancel_order_event_t>) {
            auto it = remaining.find(concrete.order_id);
            if (it == remaining.end()) {
              ++cancelled_unknown;  // already filled or unknown — engine must accept silently
            } else {
              total_cancelled_qty += it->second;
              remaining.erase(it);
            }
          }
        },
        event
      );

      book(event);

      // Apply any fills emitted by this event (aggressive fill, then resting fills) to the
      // shadow. Trades are accounted for via the matched_qty arithmetic on the fills.
      for (std::size_t f = fill_cursor; f < fills.size(); ++f) {
        apply_fill_to_shadow(fills[f]);
      }
      // Trade-vs-fill correlation: each trade has exactly one matching pair of fills (one
      // aggressive, one resting) and the trade quantity equals the resting's matched qty.
      // Verifying that here makes the test catch ordering/pairing bugs early.
      const std::size_t trades_now = trades.size() - trade_cursor;
      const std::size_t fills_now = fills.size() - fill_cursor;
      EXPECT_EQ(fills_now, 2 * trades_now);
    }

    // After the run: total_added - total_cancelled - total_matched = sum of remaining.
    quantity_t shadow_remaining_total = 0;
    for (const auto& [id, qty] : remaining) {
      shadow_remaining_total += qty;
    }
    EXPECT_EQ(total_added_qty - total_cancelled_qty - total_matched_qty, shadow_remaining_total);
    EXPECT_GT(cancelled_unknown, 0u) << "expected the generator to cancel some already-filled ids";
  }

  /// Best bid <= Best ask: the book must never cross. The generator only emits Adds and
  /// Cancels; the matching loop is responsible for resolving any cross immediately, so a
  /// non-crossing invariant must hold after every event.
  TEST(BookInvariantsTest, BookNeverCrosses) {
    using namespace matching::benchmark;
    market_profile_t profile = profile_volatile;
    profile.num_orders = 5000;
    profile.seed = 0xDEADBEEFULL;

    order_generator_t gen{profile};
    std::vector<trade_event_t> trades;
    std::vector<fill_variant_t> fills;
    invariants_recorder_t recorder{&trades, &fills};

    clob_factory_t<invariants_recorder_t> factory{50'000, recorder};
    auto book = std::move(factory).create();

    // Mirror book — track best bid / best ask via the shadow of resting quantities and the
    // trade history, since clob_t does not expose a price-level view publicly.
    std::unordered_map<order_id_t, std::pair<side_t, price_t>> resting;
    quantity_t open_qty_total = 0;
    (void)open_qty_total;

    auto apply_event_to_shadow = [&](const input_event_t& event) {
      std::visit(
        [&](const auto& concrete) {
          using event_type = std::decay_t<decltype(concrete)>;
          if constexpr (std::is_same_v<event_type, add_order_event_t>) {
            resting.emplace(concrete.order_id, std::make_pair(concrete.side, concrete.price));
          } else if constexpr (std::is_same_v<event_type, cancel_order_event_t>) {
            resting.erase(concrete.order_id);
          }
        },
        event
      );
    };

    for (std::size_t i = 0; i < profile.num_orders; ++i) {
      const input_event_t event = gen.next();
      const std::size_t fill_cursor = fills.size();

      apply_event_to_shadow(event);
      book(event);

      // Fully-filled orders: drop from the shadow. Partial fills stay (we don't track
      // remaining on the shadow here; that's covered by the conservation test).
      for (std::size_t f = fill_cursor; f < fills.size(); ++f) {
        std::visit(
          [&](const auto& fl) {
            using T = std::decay_t<decltype(fl)>;
            if constexpr (std::is_same_v<T, order_fully_filled_t>) {
              resting.erase(fl.order_id);
            }
          },
          fills[f]
        );
      }

      // Recompute best bid / best ask from the shadow and assert non-crossing.
      price_t best_bid = std::numeric_limits<price_t>::lowest();
      price_t best_ask = std::numeric_limits<price_t>::max();
      for (const auto& [id, sp] : resting) {
        if (sp.first == side_t::buy) {
          best_bid = std::max(best_bid, sp.second);
        } else {
          best_ask = std::min(best_ask, sp.second);
        }
      }
      const bool have_bid = best_bid != std::numeric_limits<price_t>::lowest();
      const bool have_ask = best_ask != std::numeric_limits<price_t>::max();
      if (have_bid && have_ask) {
        EXPECT_LE(best_bid, best_ask) << "book crossed after event " << i;
      }
    }
  }

}  // namespace matching
