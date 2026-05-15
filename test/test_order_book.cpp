#include <gtest/gtest.h>

#include <utility>
#include <variant>
#include <vector>

#include "matching/input_event.hpp"
#include "matching/order_book.hpp"
#include "matching/output_event.hpp"

namespace matching {

  using output_payload_t = std::variant<trade_event_t, order_fully_filled_t, order_partially_filled_t>;

  // Collector emitter: stores every output event in arrival order so tests can assert both
  // contents and ordering.
  struct event_record_t {
    output_payload_t payload;
  };

  struct recorder_t {
    std::vector<event_record_t>* events{nullptr};

    void operator()(const trade_event_t& e) const {
      events->push_back({e});
    }
    void operator()(const order_fully_filled_t& e) const {
      events->push_back({e});
    }
    void operator()(const order_partially_filled_t& e) const {
      events->push_back({e});
    }
  };

  struct OrderBookTest : ::testing::Test {
    std::vector<event_record_t> events_;

    auto make_book(std::size_t capacity = 1024) {
      return clob_t<recorder_t>{clob_memory_t{capacity}, recorder_t{&events_}};
    }

    static add_order_event_t buy(order_id_t id, quantity_t qty, price_t px) {
      return add_order_event_t{id, side_t::buy, qty, px};
    }
    static add_order_event_t sell(order_id_t id, quantity_t qty, price_t px) {
      return add_order_event_t{id, side_t::sell, qty, px};
    }

    static const trade_event_t* as_trade(const event_record_t& r) {
      return std::get_if<trade_event_t>(&r.payload);
    }
    static const order_fully_filled_t* as_full(const event_record_t& r) {
      return std::get_if<order_fully_filled_t>(&r.payload);
    }
    static const order_partially_filled_t* as_partial(const event_record_t& r) {
      return std::get_if<order_partially_filled_t>(&r.payload);
    }

    [[nodiscard]] static order_id_t fill_order_id(const event_record_t& r) {
      if (const auto* f = as_full(r)) {
        return f->order_id;
      }
      if (const auto* p = as_partial(r)) {
        return p->order_id;
      }
      ADD_FAILURE() << "expected fill event";
      return 0;
    }
  };

  TEST_F(OrderBookTest, AddOrderNoMatchProducesNoOutput) {
    auto book = make_book();
    book(buy(1, 10, 100));
    book(sell(2, 10, 200));
    EXPECT_TRUE(events_.empty());
    EXPECT_EQ(book.bid_depth(), 1u);
    EXPECT_EQ(book.ask_depth(), 1u);
  }

  TEST_F(OrderBookTest, BetterPriceMatchesFirst) {
    auto book = make_book();
    book(sell(1, 5, 110));
    book(sell(2, 5, 100));
    book(sell(3, 5, 105));
    book(buy(99, 5, 200));

    ASSERT_EQ(events_.size(), 3u);
    const auto* trade = as_trade(events_[0]);
    ASSERT_NE(trade, nullptr);
    EXPECT_EQ(trade->price, 100);  // Best (lowest) ask should match first.
    EXPECT_EQ(trade->quantity, 5);

    ASSERT_NE(as_full(events_[1]), nullptr);
    EXPECT_EQ(fill_order_id(events_[1]), 99);

    ASSERT_NE(as_full(events_[2]), nullptr);
    EXPECT_EQ(fill_order_id(events_[2]), 2);
  }

  TEST_F(OrderBookTest, TimePriorityWithinSamePriceLevel) {
    auto book = make_book();
    book(buy(1, 5, 100));
    book(buy(2, 5, 100));
    book(sell(99, 5, 100));

    ASSERT_EQ(events_.size(), 3u);
    ASSERT_NE(as_full(events_[2]), nullptr);
    EXPECT_EQ(fill_order_id(events_[2]), 1);  // Older order matches first.
  }

  TEST_F(OrderBookTest, AggressivePartialFillRestingFull) {
    auto book = make_book();
    book(sell(1, 3, 100));
    book(buy(2, 5, 100));

    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 3);
    ASSERT_NE(as_partial(events_[1]), nullptr);
    EXPECT_EQ(as_partial(events_[1])->remaining_quantity, 2);
    EXPECT_EQ(as_partial(events_[1])->order_id, 2);
    ASSERT_NE(as_full(events_[2]), nullptr);
    EXPECT_EQ(as_full(events_[2])->order_id, 1);
    // Residual of aggressive becomes a new resting order.
    EXPECT_EQ(book.bid_depth(), 1u);
    EXPECT_EQ(book.ask_depth(), 0u);
  }

  TEST_F(OrderBookTest, AggressiveFullRestingPartial) {
    auto book = make_book();
    book(sell(1, 10, 100));
    book(buy(2, 4, 100));

    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 4);
    ASSERT_NE(as_full(events_[1]), nullptr);
    EXPECT_EQ(as_full(events_[1])->order_id, 2);
    ASSERT_NE(as_partial(events_[2]), nullptr);
    EXPECT_EQ(as_partial(events_[2])->remaining_quantity, 6);
    EXPECT_EQ(as_partial(events_[2])->order_id, 1);
  }

  TEST_F(OrderBookTest, MultiLevelSweep) {
    auto book = make_book();
    book(sell(1, 5, 100));
    book(sell(2, 5, 101));
    book(sell(3, 5, 102));
    book(buy(99, 12, 102));

    // 3 levels touched -> 3 trades, 3 aggressive fills, 3 resting fills.
    ASSERT_EQ(events_.size(), 9u);
    EXPECT_EQ(as_trade(events_[0])->price, 100);
    EXPECT_EQ(as_trade(events_[3])->price, 101);
    EXPECT_EQ(as_trade(events_[6])->price, 102);
    EXPECT_EQ(as_trade(events_[6])->quantity, 2);

    ASSERT_NE(as_full(events_[2]), nullptr);   // resting 1
    ASSERT_NE(as_full(events_[5]), nullptr);   // resting 2
    ASSERT_NE(as_partial(events_[8]), nullptr);// resting 3 with 3 left
    EXPECT_EQ(as_partial(events_[8])->remaining_quantity, 3);
  }

  TEST_F(OrderBookTest, AggressiveResidualRestsOnBookAndIsMatchableLater) {
    auto book = make_book();
    book(sell(1, 3, 100));
    book(buy(2, 5, 100));    // matches 3, 2 remaining at 100
    events_.clear();

    book(sell(3, 2, 100));   // crosses the residual
    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 2);
    EXPECT_EQ(as_trade(events_[0])->price, 100);
    EXPECT_EQ(fill_order_id(events_[1]), 3);
    EXPECT_EQ(fill_order_id(events_[2]), 2);
    EXPECT_EQ(book.bid_depth(), 0u);
    EXPECT_EQ(book.ask_depth(), 0u);
  }

  TEST_F(OrderBookTest, CancelRemovesRestingOrder) {
    auto book = make_book();
    book(buy(1, 5, 100));
    book(cancel_order_event_t{1});

    EXPECT_TRUE(events_.empty());
    EXPECT_EQ(book.bid_depth(), 0u);

    book(sell(2, 5, 100));  // No counter-party left; should rest.
    EXPECT_TRUE(events_.empty());
    EXPECT_EQ(book.ask_depth(), 1u);
  }

  TEST_F(OrderBookTest, CancelOfUnknownIdIsNoop) {
    auto book = make_book();
    book(cancel_order_event_t{42});
    book(buy(1, 5, 100));
    book(cancel_order_event_t{99});

    EXPECT_TRUE(events_.empty());
    EXPECT_EQ(book.bid_depth(), 1u);
  }

  TEST_F(OrderBookTest, NoMatchWhenCrossConditionNotMet) {
    auto book = make_book();
    book(buy(1, 5, 99));
    book(sell(2, 5, 100));
    EXPECT_TRUE(events_.empty());
    EXPECT_EQ(book.bid_depth(), 1u);
    EXPECT_EQ(book.ask_depth(), 1u);
  }

  TEST_F(OrderBookTest, OutputOrderingTradeAggressiveResting) {
    // Trade → aggressive fill → resting fill for every match step.
    auto book = make_book();
    book(sell(10, 3, 100));
    book(sell(11, 3, 100));
    book(buy(99, 5, 100));

    ASSERT_EQ(events_.size(), 6u);
    // Match 1
    ASSERT_NE(as_trade(events_[0]), nullptr);
    ASSERT_NE(as_partial(events_[1]), nullptr);
    EXPECT_EQ(as_partial(events_[1])->order_id, 99);
    EXPECT_EQ(as_partial(events_[1])->remaining_quantity, 2);
    ASSERT_NE(as_full(events_[2]), nullptr);
    EXPECT_EQ(as_full(events_[2])->order_id, 10);
    // Match 2
    ASSERT_NE(as_trade(events_[3]), nullptr);
    ASSERT_NE(as_full(events_[4]), nullptr);
    EXPECT_EQ(fill_order_id(events_[4]), 99);
    ASSERT_NE(as_partial(events_[5]), nullptr);
    EXPECT_EQ(as_partial(events_[5])->order_id, 11);
    EXPECT_EQ(as_partial(events_[5])->remaining_quantity, 1);
  }

  TEST_F(OrderBookTest, BundledSampleFinalOrderProducesExpectedSequence) {
    auto book = make_book();
    book(sell(1'000'000, 1, 1075));
    book(buy(1'000'001, 9, 1000));
    book(buy(1'000'002, 30, 975));
    book(sell(1'000'003, 10, 1050));
    book(buy(1'000'004, 10, 950));
    book(sell(1'000'005, 2, 1025));
    book(buy(1'000'006, 1, 1000));
    book(cancel_order_event_t{1'000'004});
    book(sell(1'000'007, 5, 1025));
    EXPECT_TRUE(events_.empty());

    // The aggressive buy of 3 at 1050 against asks {1000005=2@1025, 1000007=5@1025, ...}.
    book(buy(1'000'008, 3, 1050));

    ASSERT_EQ(events_.size(), 6u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 2);
    EXPECT_EQ(as_trade(events_[0])->price, 1025);
    ASSERT_NE(as_partial(events_[1]), nullptr);
    EXPECT_EQ(as_partial(events_[1])->order_id, 1'000'008);
    EXPECT_EQ(as_partial(events_[1])->remaining_quantity, 1);
    ASSERT_NE(as_full(events_[2]), nullptr);
    EXPECT_EQ(as_full(events_[2])->order_id, 1'000'005);
    EXPECT_EQ(as_trade(events_[3])->quantity, 1);
    EXPECT_EQ(as_trade(events_[3])->price, 1025);
    ASSERT_NE(as_full(events_[4]), nullptr);
    EXPECT_EQ(as_full(events_[4])->order_id, 1'000'008);
    ASSERT_NE(as_partial(events_[5]), nullptr);
    EXPECT_EQ(as_partial(events_[5])->order_id, 1'000'007);
    EXPECT_EQ(as_partial(events_[5])->remaining_quantity, 4);
  }

  TEST_F(OrderBookTest, AggressiveOnSellSideAlsoOrderedCorrectly) {
    auto book = make_book();
    book(buy(1, 4, 100));
    book(buy(2, 4, 100));
    book(sell(99, 6, 100));
    ASSERT_EQ(events_.size(), 6u);
    EXPECT_EQ(as_trade(events_[0])->price, 100);
    ASSERT_NE(as_partial(events_[1]), nullptr);
    EXPECT_EQ(as_partial(events_[1])->order_id, 99);
    EXPECT_EQ(as_partial(events_[1])->remaining_quantity, 2);
    EXPECT_EQ(fill_order_id(events_[2]), 1);
    ASSERT_NE(as_full(events_[2]), nullptr);
    EXPECT_EQ(as_trade(events_[3])->quantity, 2);
    EXPECT_EQ(fill_order_id(events_[4]), 99);
    ASSERT_NE(as_full(events_[4]), nullptr);
    EXPECT_EQ(fill_order_id(events_[5]), 2);
    ASSERT_NE(as_partial(events_[5]), nullptr);
    EXPECT_EQ(as_partial(events_[5])->remaining_quantity, 2);
  }

  TEST_F(OrderBookTest, CancelAfterFullFillIsNoop) {
    auto book = make_book();
    book(buy(1, 5, 100));
    book(sell(2, 5, 100));   // 1 is fully filled.
    events_.clear();
    book(cancel_order_event_t{1});  // Should be silently ignored.
    EXPECT_TRUE(events_.empty());
  }

  TEST_F(OrderBookTest, ZeroQuantityAddIsNoop) {
    auto book = make_book();
    book(add_order_event_t{1, side_t::buy, 0, 100});
    EXPECT_TRUE(events_.empty());
    EXPECT_EQ(book.bid_depth(), 0u);
  }

}  // namespace matching
