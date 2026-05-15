#pragma once

#include <ostream>
#include <print>

#include "matching/message_types.hpp"
#include "matching/output_event.hpp"

namespace matching {

  /// Wire format: one LF-terminated line per event (@c TRD / @c FFL / @c PFL / @c ERR for @ref output_event_t payload
  /// rows; third field of @c ERR encodes @ref order_error_kind_t).
  class output_formatter_t {
  public:
    explicit output_formatter_t(std::ostream& out) noexcept : out_(&out) {}

    void operator()(const trade_event_t& event) const {
      std::println(*out_, "{},{},{}", wire_trade, event.quantity, event.price);
    }

    void operator()(const order_fully_filled_t& event) const {
      std::println(*out_, "{},{}", wire_fully_filled, event.order_id);
    }

    void operator()(const order_partially_filled_t& event) const {
      std::println(*out_, "{},{},{}", wire_partially_filled, event.order_id, event.remaining_quantity);
    }

    void operator()(const order_error_event_t& event) const {
      std::println(*out_, "{},{},{}", wire_error, event.order_id, static_cast<unsigned>(event.kind));
    }

    [[nodiscard]] std::ostream& sink() const noexcept {
      return *out_;
    }

  private:
    std::ostream* out_{nullptr};
  };

}  // namespace matching
