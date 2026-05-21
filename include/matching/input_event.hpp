#pragma once

#include <variant>

#include "matching/message_types.hpp"

namespace matching {

  /// Add order event (wire @c ADD,id,side,qty,price,tif): submit a new order to the matcher with
  /// an explicit time-in-force (@ref tif_t). The matcher emits exactly one output event per
  /// add_order_event_t describing the order's terminal state.
  struct add_order_event_t {
    order_id_t order_id{};
    side_t side{};
    quantity_t quantity{};
    price_t price{};
    tif_t tif{};
  };

  /// Cancel order event (wire @c CXL,id): remove a previously added order from the book. The
  /// matcher emits exactly one output event in response (@c CAN on success, @c REJ on failure).
  struct cancel_order_event_t {
    order_id_t order_id{};
  };

  /// Parser and matcher queues carry input events plus the in-band shutdown sentinel.
  using input_event_t = std::variant<add_order_event_t, cancel_order_event_t, shutdown_t>;

}  // namespace matching
