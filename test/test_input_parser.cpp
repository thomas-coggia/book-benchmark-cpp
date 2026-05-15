#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "matching/input_event.hpp"
#include "matching/input_parser.hpp"

namespace matching {

  struct captured_err_t {
    std::ostringstream stream;

    [[nodiscard]] std::ostream& sink() noexcept {
      return stream;
    }

    [[nodiscard]] std::string contents() const {
      return stream.str();
    }
  };

  TEST(ParserTest, ParsesValidAddOrder) {
    captured_err_t err;
    input_event_t evt{};
    const auto status = parse_line("ADD,42,SLL,7,1234", evt, err.sink());
    ASSERT_EQ(status, parse_status_t::ok);
    ASSERT_TRUE(std::holds_alternative<add_order_event_t>(evt));
    const auto& add = std::get<add_order_event_t>(evt);
    EXPECT_EQ(add.order_id, 42);
    EXPECT_EQ(add.side, side_t::sell);
    EXPECT_EQ(add.quantity, 7);
    EXPECT_EQ(add.price, 1234);
    EXPECT_TRUE(err.contents().empty());
  }

  TEST(ParserTest, ParsesValidCancelOrder) {
    captured_err_t err;
    input_event_t evt{};
    const auto status = parse_line("CXL,42", evt, err.sink());
    ASSERT_EQ(status, parse_status_t::ok);
    ASSERT_TRUE(std::holds_alternative<cancel_order_event_t>(evt));
    EXPECT_EQ(std::get<cancel_order_event_t>(evt).order_id, 42);
    EXPECT_TRUE(err.contents().empty());
  }

  TEST(ParserTest, BlankLineIsSilentlySkipped) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("", evt, err.sink()), parse_status_t::skipped);
    EXPECT_EQ(parse_line("   \t  ", evt, err.sink()), parse_status_t::skipped);
    EXPECT_TRUE(err.contents().empty());
  }

  TEST(ParserTest, CommentLineIsSilentlySkipped) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("# this is a comment", evt, err.sink()), parse_status_t::skipped);
    EXPECT_TRUE(err.contents().empty());
  }

  TEST(ParserTest, UnknownMessageTypeReportsError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("NOT_A_RECORD", evt, err.sink()), parse_status_t::error);
    const std::string captured = err.contents();
    EXPECT_NE(captured.find("Unknown message type: NOT_A_RECORD"), std::string::npos);
  }

  TEST(ParserTest, UnknownOpcodeReportsError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("ZZZ,1,BUY,2,3,4", evt, err.sink()), parse_status_t::error);
    EXPECT_NE(err.contents().find("Unknown message type: ZZZ"), std::string::npos);
  }

  TEST(ParserTest, MissingFieldsInAddReportError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("ADD,1,BUY,5", evt, err.sink()), parse_status_t::error);
    EXPECT_NE(err.contents().find("Ill-formed AddOrderRequest"), std::string::npos);
  }

  TEST(ParserTest, NonNumericFieldInAddReportsError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("ADD,abc,BUY,5,100", evt, err.sink()), parse_status_t::error);
    EXPECT_NE(err.contents().find("Ill-formed AddOrderRequest"), std::string::npos);
  }

  TEST(ParserTest, InvalidSideTokenReportsError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("ADD,1,H,5,100", evt, err.sink()), parse_status_t::error);
    EXPECT_NE(err.contents().find("Invalid side token"), std::string::npos);
  }

  TEST(ParserTest, NonPositiveQuantityReportsError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("ADD,1,BUY,0,100", evt, err.sink()), parse_status_t::error);
    EXPECT_NE(err.contents().find("Non-positive"), std::string::npos);
  }

  TEST(ParserTest, NonPositivePriceReportsError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("ADD,1,BUY,1,0", evt, err.sink()), parse_status_t::error);
    EXPECT_NE(err.contents().find("Non-positive"), std::string::npos);
  }

  TEST(ParserTest, IllFormedCancelReportsError) {
    captured_err_t err;
    input_event_t evt{};
    EXPECT_EQ(parse_line("CXL,abc", evt, err.sink()), parse_status_t::error);
    EXPECT_NE(err.contents().find("Ill-formed CancelOrderRequest"), std::string::npos);
  }

  TEST(ParserTest, ParseStreamRoutesEventsAndPreservesOrder) {
    captured_err_t err;
    const std::string in_str =
      "ADD,1,BUY,5,100\n"
      "NOT_A_RECORD\n"
      "CXL,1\n"
      "# trailing comment\n";
    std::istringstream in{in_str};

    std::vector<input_event_t> collected;
    parse_stream(in, [&](const input_event_t& e) { collected.push_back(e); }, err.sink());

    ASSERT_EQ(collected.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<add_order_event_t>(collected[0]));
    EXPECT_TRUE(std::holds_alternative<cancel_order_event_t>(collected[1]));
    EXPECT_NE(err.contents().find("Unknown message type: NOT_A_RECORD"), std::string::npos);
  }

  TEST(ParserTest, RobustnessNoCrashOnGarbage) {
    captured_err_t err;
    input_event_t evt{};
    // Garbage lines; none must crash.
    EXPECT_EQ(parse_line(",,,,,", evt, err.sink()), parse_status_t::error);
    EXPECT_EQ(parse_line("0", evt, err.sink()), parse_status_t::error);
    EXPECT_EQ(parse_line("ADD,1", evt, err.sink()), parse_status_t::error);
    EXPECT_EQ(parse_line("CXL,", evt, err.sink()), parse_status_t::error);
    EXPECT_EQ(parse_line("ADD,1,BUY,2,3,extra", evt, err.sink()), parse_status_t::error);
    SUCCEED();
  }

}  // namespace matching
