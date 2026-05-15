#pragma once

#include <variant>

#include "matching/message_types.hpp"

namespace matching {

  /// TradeEvent (msgtype 2): a fill between an aggressive and a resting order at the resting price.
  struct trade_event_t {
    quantity_t quantity{};
    price_t price{};
  };

  /// OrderFullyFilled (msgtype 3): order removed from the book after a trade.
  struct order_fully_filled_t {
    order_id_t order_id{};
  };

  /// OrderPartiallyFilled (msgtype 4): order still resting with updated quantity.
  struct order_partially_filled_t {
    order_id_t order_id{};
    quantity_t remaining_quantity{};
  };

  /// Semantic category for @ref order_error_event_t (encoded as an integer on the wire).
  enum class order_error_kind_t : std::uint8_t {
    duplicate_order_id = 0,
    unknown_order_id = 1,
  };

  /// Non-fatal input rejection surfaced on the matcher → writer queue for downstream visibility.
  struct order_error_event_t {
    order_id_t order_id{};
    order_error_kind_t kind{};
  };

  /// Matcher → writer queue: wire-format output messages plus @ref shutdown_t (never serialised).
  using output_event_t =
    std::variant<trade_event_t, order_fully_filled_t, order_partially_filled_t, order_error_event_t, shutdown_t>;

}  // namespace matching
