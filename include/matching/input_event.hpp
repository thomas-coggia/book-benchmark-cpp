#pragma once

#include <variant>

#include "matching/message_types.hpp"

namespace matching {

  /// Add order event (msgtype 0): submit a new resting/aggressive order.
  struct add_order_event_t {
    order_id_t order_id{};
    side_t side{};
    quantity_t quantity{};
    price_t price{};
  };

  /// Cancel order event (msgtype 1): remove a previously added order from the book.
  struct cancel_order_event_t {
    order_id_t order_id{};
  };

  /// Parser and matcher queues carry input events plus the in-band shutdown sentinel.
  using input_event_t = std::variant<add_order_event_t, cancel_order_event_t, shutdown_t>;

}  // namespace matching
