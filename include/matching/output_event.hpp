#pragma once

#include <variant>

#include "matching/message_types.hpp"

namespace matching {

  /// Wire @c TRD: bilateral trade between aggressive and resting orders (quantity only).
  struct trade_event_t {
    order_id_t aggressive_order_id{};
    order_id_t resting_order_id{};
    quantity_t quantity{};
  };

  /// Reasons an order may be rejected by the matcher (third field of @ref wire_rejected lines).
  ///
  /// Wire encoding (3-letter strings, see @ref message_types.hpp):
  ///   * @c duplicate_order_id   — @ref wire_reject_duplicate_id (DUP)
  ///   * @c unknown_order_id     — @ref wire_reject_unknown_id   (UNK)
  ///   * @c invalid_quantity     — @ref wire_reject_invalid_quantity (IQT)
  ///   * @c invalid_price        — @ref wire_reject_invalid_price    (IPR)
  ///   * @c invalid_order_id     — @ref wire_reject_invalid_order_id (IID)
  enum class reject_code_t : std::uint8_t {
    duplicate_order_id = 0,
    unknown_order_id = 1,
    invalid_quantity = 2,
    invalid_price = 3,
    invalid_order_id = 4,
  };

  /// Reasons an order may end in the @c Cancelled terminal state.
  ///
  /// Wire encoding (last field of @ref wire_cancelled lines):
  ///   * @c user_request        — @ref wire_cause_user (USR): explicit @c CXL.
  ///   * @c immediate_or_cancel — @ref wire_cause_ioc  (IOC): IOC residue after matching.
  ///   * @c fill_or_kill        — @ref wire_cause_fok  (FOK): FOK could not be fully filled.
  enum class cancel_cause_t : std::uint8_t {
    user_request = 0,
    immediate_or_cancel = 1,
    fill_or_kill = 2,
  };

  /// Wire @c RST: order accepted and (at least partially) resting on the book.
  struct order_resting_event_t {
    order_id_t order_id{};
    quantity_t filled_quantity{};
    quantity_t resting_quantity{};
  };

  /// Wire @c FIL: order fully filled (no residue rests, none cancelled).
  struct order_filled_event_t {
    order_id_t order_id{};
    quantity_t filled_quantity{};
  };

  /// Wire @c CAN: order terminated with a cancelled portion.
  struct order_cancelled_event_t {
    order_id_t order_id{};
    quantity_t filled_quantity{};
    quantity_t cancelled_quantity{};
    cancel_cause_t cause{};
  };

  /// Wire @c REJ: order rejected; no state change to the book.
  struct order_rejected_event_t {
    order_id_t order_id{};
    reject_code_t reject_code{};
  };

  /// Matcher→writer variant (@ref shutdown_t is control-plane only).
  using output_event_t = std::variant<
    trade_event_t,
    order_resting_event_t,
    order_filled_event_t,
    order_cancelled_event_t,
    order_rejected_event_t,
    shutdown_t>;

}  // namespace matching
