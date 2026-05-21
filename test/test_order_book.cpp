#include <gtest/gtest.h>

#include <utility>
#include <variant>
#include <vector>

#include "matching/input_event.hpp"
#include "matching/order_book.hpp"
#include "matching/output_event.hpp"

namespace matching {

  using output_payload_t = std::variant<
    trade_event_t,
    order_resting_event_t,
    order_filled_event_t,
    order_cancelled_event_t,
    order_rejected_event_t>;

  struct event_record_t {
    output_payload_t payload;
  };

  struct recorder_t {
    std::vector<event_record_t>* events{nullptr};

    void operator()(const trade_event_t& e) const {
      events->push_back({e});
    }

    void operator()(const order_resting_event_t& e) const {
      events->push_back({e});
    }

    void operator()(const order_filled_event_t& e) const {
      events->push_back({e});
    }

    void operator()(const order_cancelled_event_t& e) const {
      events->push_back({e});
    }

    void operator()(const order_rejected_event_t& e) const {
      events->push_back({e});
    }
  };

  struct OrderBookTest : ::testing::Test {
    std::vector<event_record_t> events_;

    auto make_book(std::size_t capacity = 1024) {
      return clob_t<recorder_t>{clob_memory_t{capacity}, recorder_t{&events_}};
    }

    static add_order_event_t buy(order_id_t id, quantity_t qty, price_t px, tif_t tif = tif_t::gtc) {
      return add_order_event_t{id, side_t::buy, qty, px, tif};
    }

    static add_order_event_t sell(order_id_t id, quantity_t qty, price_t px, tif_t tif = tif_t::gtc) {
      return add_order_event_t{id, side_t::sell, qty, px, tif};
    }

    static const trade_event_t* as_trade(const event_record_t& r) {
      return std::get_if<trade_event_t>(&r.payload);
    }

    static const order_resting_event_t* as_resting(const event_record_t& r) {
      return std::get_if<order_resting_event_t>(&r.payload);
    }

    static const order_filled_event_t* as_filled(const event_record_t& r) {
      return std::get_if<order_filled_event_t>(&r.payload);
    }

    static const order_cancelled_event_t* as_cancelled(const event_record_t& r) {
      return std::get_if<order_cancelled_event_t>(&r.payload);
    }

    static const order_rejected_event_t* as_rejected(const event_record_t& r) {
      return std::get_if<order_rejected_event_t>(&r.payload);
    }
  };

  TEST_F(OrderBookTest, AddOrderNoMatchEmitsRestingFullQuantity) {
    auto book = make_book();
    book(buy(1, 10, 100));
    ASSERT_EQ(events_.size(), 1u);
    const auto* rst = as_resting(events_[0]);
    ASSERT_NE(rst, nullptr);
    EXPECT_EQ(rst->order_id, 1);
    EXPECT_EQ(rst->filled_quantity, 0);
    EXPECT_EQ(rst->resting_quantity, 10);
    EXPECT_EQ(book.bid_depth(), 1u);
    EXPECT_EQ(book.ask_depth(), 0u);
  }

  TEST_F(OrderBookTest, BetterPriceMatchesFirstAndEmitsMatchingTradeThenFilled) {
    auto book = make_book();
    book(sell(1, 5, 110));
    book(sell(2, 5, 100));
    book(sell(3, 5, 105));
    events_.clear();
    book(buy(99, 5, 200));

    ASSERT_EQ(events_.size(), 3u);
    const auto* trd = as_trade(events_[0]);
    ASSERT_NE(trd, nullptr);
    EXPECT_EQ(trd->aggressive_order_id, 99);
    EXPECT_EQ(trd->resting_order_id, 2);
    EXPECT_EQ(trd->quantity, 5);
    ASSERT_NE(as_filled(events_[1]), nullptr);
    EXPECT_EQ(as_filled(events_[1])->order_id, 2);
    ASSERT_NE(as_filled(events_[2]), nullptr);
    EXPECT_EQ(as_filled(events_[2])->order_id, 99);
  }

  TEST_F(OrderBookTest, AggressivePartialFillRestingResidueEmitsTradeThenResting) {
    auto book = make_book();
    book(sell(1, 3, 100));
    events_.clear();
    book(buy(2, 5, 100));

    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 3);
    const auto* rst = as_resting(events_[2]);
    ASSERT_NE(rst, nullptr);
    EXPECT_EQ(rst->order_id, 2);
    EXPECT_EQ(rst->filled_quantity, 3);
    EXPECT_EQ(rst->resting_quantity, 2);
    EXPECT_EQ(book.bid_depth(), 1u);
    EXPECT_EQ(book.ask_depth(), 0u);
  }

  TEST_F(OrderBookTest, AggressiveFullFillEmitsTradeThenFilled) {
    auto book = make_book();
    book(sell(1, 10, 100));
    events_.clear();
    book(buy(2, 4, 100));

    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 4);
    const auto* fil = as_filled(events_[2]);
    ASSERT_NE(fil, nullptr);
    EXPECT_EQ(fil->order_id, 2);
    EXPECT_EQ(fil->filled_quantity, 4);
  }

  TEST_F(OrderBookTest, MultiLevelSweepEmitsTradesThenFilled) {
    auto book = make_book();
    book(sell(1, 5, 100));
    book(sell(2, 5, 101));
    book(sell(3, 5, 102));
    events_.clear();
    book(buy(99, 12, 102));

    ASSERT_EQ(events_.size(), 7u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 5);
    EXPECT_EQ(as_trade(events_[2])->quantity, 5);
    EXPECT_EQ(as_trade(events_[4])->quantity, 2);
    const auto* fil = as_filled(events_[6]);
    ASSERT_NE(fil, nullptr);
    EXPECT_EQ(fil->order_id, 99);
    EXPECT_EQ(fil->filled_quantity, 12);
  }

  TEST_F(OrderBookTest, CancelRemovesRestingOrderAndEmitsCancelled) {
    auto book = make_book();
    book(buy(1, 5, 100));
    events_.clear();

    book(cancel_order_event_t{1});
    ASSERT_EQ(events_.size(), 1u);
    const auto* can = as_cancelled(events_[0]);
    ASSERT_NE(can, nullptr);
    EXPECT_EQ(can->order_id, 1);
    EXPECT_EQ(can->filled_quantity, 0);
    EXPECT_EQ(can->cancelled_quantity, 5);
    EXPECT_EQ(can->cause, cancel_cause_t::user_request);
    EXPECT_EQ(book.bid_depth(), 0u);
  }

  TEST_F(OrderBookTest, CancelOfUnknownIdEmitsRejected) {
    auto book = make_book();
    book(cancel_order_event_t{42});
    ASSERT_EQ(events_.size(), 1u);
    EXPECT_EQ(as_rejected(events_[0])->reject_code, reject_code_t::unknown_order_id);
  }

  TEST_F(OrderBookTest, CancelAfterFullFillEmitsRejected) {
    auto book = make_book();
    book(buy(1, 5, 100));
    book(sell(2, 5, 100));
    events_.clear();
    book(cancel_order_event_t{1});
    ASSERT_EQ(events_.size(), 1u);
    EXPECT_EQ(as_rejected(events_[0])->reject_code, reject_code_t::unknown_order_id);
  }

  TEST_F(OrderBookTest, DuplicateAddIdEmitsRejected) {
    auto book = make_book();
    book(buy(1, 5, 100));
    events_.clear();
    book(buy(1, 3, 99));
    ASSERT_EQ(events_.size(), 1u);
    EXPECT_EQ(as_rejected(events_[0])->reject_code, reject_code_t::duplicate_order_id);
    EXPECT_EQ(book.bid_depth(), 1u);
  }

  TEST_F(OrderBookTest, InvalidQuantityRejected) {
    auto book = make_book();
    book(add_order_event_t{1, side_t::buy, 0, 100, tif_t::gtc});
    EXPECT_EQ(as_rejected(events_[0])->reject_code, reject_code_t::invalid_quantity);
  }

  TEST_F(OrderBookTest, InvalidPriceRejected) {
    auto book = make_book();
    book(add_order_event_t{1, side_t::buy, 10, 0, tif_t::gtc});
    EXPECT_EQ(as_rejected(events_[0])->reject_code, reject_code_t::invalid_price);
  }

  TEST_F(OrderBookTest, InvalidOrderIdOnAddRejected) {
    auto book = make_book();
    book(add_order_event_t{0, side_t::buy, 10, 100, tif_t::gtc});
    EXPECT_EQ(as_rejected(events_[0])->reject_code, reject_code_t::invalid_order_id);
  }

  TEST_F(OrderBookTest, InvalidOrderIdOnCancelRejected) {
    auto book = make_book();
    book(cancel_order_event_t{0});
    EXPECT_EQ(as_rejected(events_[0])->reject_code, reject_code_t::invalid_order_id);
  }

  TEST_F(OrderBookTest, IocFullyMatchedEmitsTradeThenFilled) {
    auto book = make_book();
    book(sell(1, 10, 100));
    events_.clear();
    book(buy(2, 5, 100, tif_t::ioc));
    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 5);
    EXPECT_EQ(as_filled(events_[2])->filled_quantity, 5);
  }

  TEST_F(OrderBookTest, IocPartialFillEmitsMatchingTradeThenCancelledWithIocCause) {
    auto book = make_book();
    book(sell(1, 3, 100));
    events_.clear();
    book(buy(2, 5, 100, tif_t::ioc));
    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 3);
    const auto* can = as_cancelled(events_[2]);
    ASSERT_NE(can, nullptr);
    EXPECT_EQ(can->filled_quantity, 3);
    EXPECT_EQ(can->cancelled_quantity, 2);
    EXPECT_EQ(can->cause, cancel_cause_t::immediate_or_cancel);
    EXPECT_EQ(book.bid_depth(), 0u);
  }

  TEST_F(OrderBookTest, IocNoMatchEmitsCancelledZeroFill) {
    auto book = make_book();
    book(sell(1, 5, 110));
    events_.clear();
    book(buy(2, 5, 100, tif_t::ioc));
    ASSERT_EQ(events_.size(), 1u);
    const auto* can = as_cancelled(events_[0]);
    ASSERT_NE(can, nullptr);
    EXPECT_EQ(can->filled_quantity, 0);
    EXPECT_EQ(can->cancelled_quantity, 5);
    EXPECT_EQ(can->cause, cancel_cause_t::immediate_or_cancel);
  }

  TEST_F(OrderBookTest, FokFullyFillableEmitsTradesThenFilled) {
    auto book = make_book();
    book(sell(1, 5, 100));
    book(sell(2, 5, 101));
    events_.clear();
    book(buy(3, 10, 101, tif_t::fok));
    ASSERT_EQ(events_.size(), 5u);
    EXPECT_EQ(as_filled(events_[4])->filled_quantity, 10);
  }

  TEST_F(OrderBookTest, FokNotFullyFillableEmitsCancelledFokCause) {
    auto book = make_book();
    book(sell(1, 4, 100));
    events_.clear();
    book(buy(2, 5, 100, tif_t::fok));
    ASSERT_EQ(events_.size(), 1u);
    const auto* can = as_cancelled(events_[0]);
    ASSERT_NE(can, nullptr);
    EXPECT_EQ(can->filled_quantity, 0);
    EXPECT_EQ(can->cancelled_quantity, 5);
    EXPECT_EQ(can->cause, cancel_cause_t::fill_or_kill);
    EXPECT_EQ(book.ask_depth(), 1u);
  }

  TEST_F(OrderBookTest, FokIgnoresOppositeSideAboveLimit) {
    auto book = make_book();
    book(sell(1, 3, 100));
    book(sell(2, 2, 105));
    events_.clear();
    book(buy(3, 5, 100, tif_t::fok));
    ASSERT_EQ(events_.size(), 1u);
    EXPECT_EQ(as_cancelled(events_[0])->cause, cancel_cause_t::fill_or_kill);
    EXPECT_EQ(book.ask_depth(), 2u);
  }

  TEST_F(OrderBookTest, GtcResidueRestsAndIsLaterMatchable) {
    auto book = make_book();
    book(sell(1, 3, 100));
    book(buy(2, 5, 100));
    events_.clear();
    book(sell(3, 2, 100));
    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 2);
    EXPECT_EQ(as_filled(events_[2])->order_id, 3);
    EXPECT_EQ(book.bid_depth(), 0u);
  }

  TEST_F(OrderBookTest, TimePriorityWithinSamePriceLevel) {
    auto book = make_book();
    book(buy(1, 5, 100));
    book(buy(2, 5, 100));
    events_.clear();
    book(sell(99, 5, 100));
    ASSERT_EQ(events_.size(), 3u);
    EXPECT_NE(as_trade(events_[0]), nullptr);
    events_.clear();
    book(sell(100, 5, 100));
    ASSERT_EQ(events_.size(), 3u);
    EXPECT_EQ(book.bid_depth(), 0u);
  }

  TEST_F(OrderBookTest, NoMatchWhenCrossConditionNotMet) {
    auto book = make_book();
    book(buy(1, 5, 99));
    events_.clear();
    book(sell(2, 5, 100));
    ASSERT_EQ(events_.size(), 1u);
    const auto* rst = as_resting(events_[0]);
    ASSERT_NE(rst, nullptr);
    EXPECT_EQ(rst->filled_quantity, 0);
    EXPECT_EQ(rst->resting_quantity, 5);
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
    events_.clear();
    book(buy(1'000'008, 3, 1050));
    ASSERT_EQ(events_.size(), 5u);
    EXPECT_EQ(as_trade(events_[0])->quantity, 2);
    EXPECT_EQ(as_filled(events_[1])->order_id, 1'000'005);
    EXPECT_EQ(as_trade(events_[2])->quantity, 1);
    EXPECT_EQ(as_resting(events_[3])->order_id, 1'000'007);
    EXPECT_EQ(as_filled(events_[4])->order_id, 1'000'008);
    EXPECT_EQ(as_filled(events_[4])->filled_quantity, 3);
  }

}  // namespace matching
