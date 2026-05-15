#pragma once

#include <ostream>
#include <print>

#include "matching/output_event.hpp"

namespace matching {

  /// Stateless formatter that turns the engine's typed output events into the textual
  /// wire format. Each call writes exactly one line (CR-less LF) to the bound
  /// @c std::ostream sink, in the order @c "<msgtype>,<field>...".
  ///
  /// Held as a value type so a callable @c clob_t emitter can wrap it cheaply
  /// and the matching loop keeps fully inlined formatting semantics.
  class output_formatter_t {
  public:
    explicit output_formatter_t(std::ostream& out) noexcept : out_(&out) {}

    /// TradeEvent → @c "2,<quantity>,<price>"
    void operator()(const trade_event_t& event) const {
      std::println(*out_, "2,{},{}", event.quantity, event.price);
    }

    /// OrderFullyFilled → @c "3,<orderid>"
    void operator()(const order_fully_filled_t& event) const {
      std::println(*out_, "3,{}", event.order_id);
    }

    /// OrderPartiallyFilled → @c "4,<orderid>,<new_qty>"
    void operator()(const order_partially_filled_t& event) const {
      std::println(*out_, "4,{},{}", event.order_id, event.remaining_quantity);
    }

    /// OrderError → @c "5,<orderid>,<kind>" (@p kind is @ref order_error_kind_t as integer).
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
