#pragma once

#include <variant>

#include "matching/message_types.hpp"

namespace matching {

  /// Wire @c TRD: trade at resting price.
  struct trade_event_t {
    quantity_t quantity{};
    price_t price{};
  };

  /// Wire @c FFL.
  struct order_fully_filled_t {
    order_id_t order_id{};
  };

  /// Wire @c PFL.
  struct order_partially_filled_t {
    order_id_t order_id{};
    quantity_t remaining_quantity{};
  };

  /// Serialized as the third field of @c ERR lines.
  enum class order_error_kind_t : std::uint8_t {
    duplicate_order_id = 0,
    unknown_order_id = 1,
    invalid_add_quantity = 2,
    invalid_add_price = 3,
    invalid_add_order_id = 4,
    invalid_cancel_order_id = 5,
  };

  /// Wire @c ERR row payload.
  struct order_error_event_t {
    order_id_t order_id{};
    order_error_kind_t kind{};
  };

  /// Matcher→writer variant (@ref shutdown_t is control-plane only).
  using output_event_t =
    std::variant<trade_event_t, order_fully_filled_t, order_partially_filled_t, order_error_event_t, shutdown_t>;

}  // namespace matching
