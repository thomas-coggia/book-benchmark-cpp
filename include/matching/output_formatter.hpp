#pragma once

#include <ostream>
#include <print>
#include <string_view>

#include "matching/message_types.hpp"
#include "matching/output_event.hpp"

namespace matching {

  namespace detail {

    [[nodiscard]] constexpr std::string_view reject_code_token(reject_code_t code) noexcept {
      switch (code) {
      case reject_code_t::duplicate_order_id:
        return wire_reject_duplicate_id;
      case reject_code_t::unknown_order_id:
        return wire_reject_unknown_id;
      case reject_code_t::invalid_quantity:
        return wire_reject_invalid_quantity;
      case reject_code_t::invalid_price:
        return wire_reject_invalid_price;
      case reject_code_t::invalid_order_id:
        return wire_reject_invalid_order_id;
      }
      return wire_reject_invalid_order_id;
    }

    [[nodiscard]] constexpr std::string_view cancel_cause_token(cancel_cause_t cause) noexcept {
      switch (cause) {
      case cancel_cause_t::user_request:
        return wire_cause_user;
      case cancel_cause_t::immediate_or_cancel:
        return wire_cause_ioc;
      case cancel_cause_t::fill_or_kill:
        return wire_cause_fok;
      }
      return wire_cause_user;
    }

  }  // namespace detail

  /// Wire format: one LF-terminated line per @ref output_event_t alternative.
  class output_formatter_t {
  public:
    explicit output_formatter_t(std::ostream& out) noexcept : out_(&out) {}

    void operator()(const trade_event_t& event) const {
      std::println(*out_, "{},{},{},{}", wire_trade, event.aggressive_order_id, event.resting_order_id, event.quantity);
    }

    void operator()(const order_resting_event_t& event) const {
      std::println(*out_, "{},{},{},{}", wire_resting, event.order_id, event.filled_quantity, event.resting_quantity);
    }

    void operator()(const order_filled_event_t& event) const {
      std::println(*out_, "{},{},{}", wire_filled, event.order_id, event.filled_quantity);
    }

    void operator()(const order_cancelled_event_t& event) const {
      std::println(
        *out_,
        "{},{},{},{},{}",
        wire_cancelled,
        event.order_id,
        event.filled_quantity,
        event.cancelled_quantity,
        detail::cancel_cause_token(event.cause)
      );
    }

    void operator()(const order_rejected_event_t& event) const {
      std::println(*out_, "{},{},{}", wire_rejected, event.order_id, detail::reject_code_token(event.reject_code));
    }

    [[nodiscard]] std::ostream& sink() const noexcept {
      return *out_;
    }

  private:
    std::ostream* out_{nullptr};
  };

}  // namespace matching
