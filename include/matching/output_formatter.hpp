#pragma once

#include <ostream>
#include <print>

#include "matching/output_event.hpp"

namespace matching {

  /// Wire format: one LF-terminated line per event (msgtypes 2–5 for @ref output_event_t payload rows; third field
  /// encodes @ref order_error_kind_t).
  class output_formatter_t {
  public:
    explicit output_formatter_t(std::ostream& out) noexcept : out_(&out) {}

    void operator()(const trade_event_t& event) const {
      std::println(*out_, "2,{},{}", event.quantity, event.price);
    }

    void operator()(const order_fully_filled_t& event) const {
      std::println(*out_, "3,{}", event.order_id);
    }

    void operator()(const order_partially_filled_t& event) const {
      std::println(*out_, "4,{},{}", event.order_id, event.remaining_quantity);
    }

    void operator()(const order_error_event_t& event) const {
      std::println(*out_, "5,{},{}", event.order_id, static_cast<unsigned>(event.kind));
    }

    [[nodiscard]] std::ostream& sink() const noexcept {
      return *out_;
    }

  private:
    std::ostream* out_{nullptr};
  };

}  // namespace matching
