#include <gtest/gtest.h>

#include <cstdint>
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

    using terminal_event_t =
      std::variant<order_resting_event_t, order_filled_event_t, order_cancelled_event_t, order_rejected_event_t>;

    struct invariants_recorder_t {
      std::vector<trade_event_t>* trades{nullptr};
      std::vector<terminal_event_t>* terminals{nullptr};

      void operator()(const trade_event_t& e) const {
        trades->push_back(e);
      }

      void operator()(const order_resting_event_t& e) const {
        terminals->emplace_back(e);
      }

      void operator()(const order_filled_event_t& e) const {
        terminals->emplace_back(e);
      }

      void operator()(const order_cancelled_event_t& e) const {
        terminals->emplace_back(e);
      }

      void operator()(const order_rejected_event_t& e) const {
        terminals->emplace_back(e);
      }
    };

    [[nodiscard]] order_id_t terminal_order_id(const terminal_event_t& terminal) {
      return std::visit([](const auto& out) -> order_id_t { return out.order_id; }, terminal);
    }

    [[nodiscard]] quantity_t
    trade_qty_sum(const std::vector<trade_event_t>& trades, std::size_t begin, std::size_t end) {
      quantity_t sum = 0;
      for (std::size_t i = begin; i < end; ++i) {
        sum += trades[i].quantity;
      }
      return sum;
    }

    [[nodiscard]] std::size_t
    count_terminals_for(const std::vector<terminal_event_t>& terminals, std::size_t begin, order_id_t order_id) {
      std::size_t count = 0;
      for (std::size_t i = begin; i < terminals.size(); ++i) {
        if (terminal_order_id(terminals[i]) == order_id) {
          ++count;
        }
      }
      return count;
    }

  }  // namespace

  TEST(BookInvariantsTest, TerminalEventsSatisfyLocalConservation) {
    using namespace matching::benchmark;
    market_profile_t profile = profile_active_match;
    profile.num_orders = 10'000;
    profile.seed = 0xC0FFEEULL;

    order_generator_t gen{profile};
    std::vector<trade_event_t> trades;
    std::vector<terminal_event_t> terminals;
    invariants_recorder_t recorder{&trades, &terminals};

    clob_factory_t<invariants_recorder_t> factory{200'000, recorder};
    auto book = std::move(factory).create();

    std::unordered_map<order_id_t, quantity_t> input_qty;
    std::size_t unknown_cancels = 0;
    std::size_t user_cancels = 0;

    for (std::size_t i = 0; i < profile.num_orders; ++i) {
      const input_event_t input = gen.next();
      const std::size_t trade_cursor = trades.size();
      const std::size_t terminal_cursor = terminals.size();

      order_id_t input_order_id = 0;
      std::visit(
        [&](const auto& concrete) {
          using event_type = std::decay_t<decltype(concrete)>;
          if constexpr (std::is_same_v<event_type, add_order_event_t>) {
            input_order_id = concrete.order_id;
            input_qty.emplace(concrete.order_id, concrete.quantity);
          } else if constexpr (std::is_same_v<event_type, cancel_order_event_t>) {
            input_order_id = concrete.order_id;
          }
        },
        input
      );

      book(input);
      ASSERT_EQ(count_terminals_for(terminals, terminal_cursor, input_order_id), 1u);

      const bool is_cancel_input = std::holds_alternative<cancel_order_event_t>(input);
      const quantity_t traded = trade_qty_sum(trades, trade_cursor, trades.size());

      const terminal_event_t& input_terminal = [&]() -> const terminal_event_t& {
        for (std::size_t t = terminals.size(); t-- > terminal_cursor;) {
          if (terminal_order_id(terminals[t]) == input_order_id) {
            return terminals[t];
          }
        }
        ADD_FAILURE() << "missing terminal for input order " << input_order_id;
        return terminals.back();
      }();

      std::visit(
        [&](const auto& out) {
          using out_type = std::decay_t<decltype(out)>;
          if constexpr (std::is_same_v<out_type, order_resting_event_t>) {
            ASSERT_FALSE(is_cancel_input);
            const auto it = input_qty.find(out.order_id);
            ASSERT_NE(it, input_qty.end());
            EXPECT_EQ(out.filled_quantity + out.resting_quantity, it->second);
            EXPECT_EQ(out.filled_quantity, traded);
          } else if constexpr (std::is_same_v<out_type, order_filled_event_t>) {
            ASSERT_FALSE(is_cancel_input);
            const auto it = input_qty.find(out.order_id);
            ASSERT_NE(it, input_qty.end());
            EXPECT_EQ(out.filled_quantity, it->second);
            EXPECT_EQ(out.filled_quantity, traded);
          } else if constexpr (std::is_same_v<out_type, order_cancelled_event_t>) {
            EXPECT_GT(out.cancelled_quantity, 0);
            if (out.cause == cancel_cause_t::user_request) {
              ASSERT_TRUE(is_cancel_input);
              EXPECT_EQ(out.filled_quantity, 0);
              EXPECT_EQ(traded, 0);
              ++user_cancels;
            } else {
              ASSERT_FALSE(is_cancel_input);
              const auto it = input_qty.find(out.order_id);
              ASSERT_NE(it, input_qty.end());
              EXPECT_EQ(out.filled_quantity + out.cancelled_quantity, it->second);
              EXPECT_EQ(out.filled_quantity, traded);
            }
          } else if constexpr (std::is_same_v<out_type, order_rejected_event_t>) {
            EXPECT_EQ(traded, 0);
            if (out.reject_code == reject_code_t::unknown_order_id) {
              ASSERT_TRUE(is_cancel_input);
              ++unknown_cancels;
            }
          }
        },
        input_terminal
      );
    }

    EXPECT_GT(user_cancels, 0u);
    EXPECT_GT(unknown_cancels, 0u);
  }

}  // namespace matching
