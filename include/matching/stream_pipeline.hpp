#pragma once

#include <cstddef>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>

#include "matching/input_event.hpp"
#include "matching/input_parser.hpp"
#include "matching/order_book.hpp"
#include "matching/output_event.hpp"
#include "matching/output_formatter.hpp"
#include "matching/runtime/event_loop.hpp"
#include "matching/runtime/spsc_queue.hpp"

namespace matching {

  /// Routes @ref clob_t output into @ref output_event_t on the matcher→writer queue; spin yields if full or stopping.
  template <std::size_t Capacity>
  class queue_emitter_t {
  public:
    using output_queue_type = runtime::spsc_queue_t<output_event_t, Capacity>;

    queue_emitter_t(std::shared_ptr<output_queue_type> queue, std::stop_token token) noexcept
      : queue_(std::move(queue)), token_(std::move(token)) {}

    void operator()(const trade_event_t& event) noexcept {
      push(output_event_t{event});
    }

    void operator()(const order_resting_event_t& event) noexcept {
      push(output_event_t{event});
    }

    void operator()(const order_filled_event_t& event) noexcept {
      push(output_event_t{event});
    }

    void operator()(const order_cancelled_event_t& event) noexcept {
      push(output_event_t{event});
    }

    void operator()(const order_rejected_event_t& event) noexcept {
      push(output_event_t{event});
    }

  private:
    void push(const output_event_t& event) noexcept {
      while (!token_.stop_requested() && !queue_->try_push(event)) {
        runtime::cpu_pause();
      }
    }

    std::shared_ptr<output_queue_type> queue_;
    std::stop_token token_;
  };

  /// Reads lines → @ref parse_stream → order queue; ends with @ref shutdown_t.
  template <std::size_t Capacity>
  class reader_loop_t {
  public:
    using order_queue_type = runtime::spsc_queue_t<input_event_t, Capacity>;

    reader_loop_t(
      std::istream& in,
      std::ostream& err,
      std::shared_ptr<order_queue_type> queue,
      std::stop_token token
    ) noexcept
      : in_(in), err_(err), queue_(std::move(queue)), token_(std::move(token)) {}

    void run() {
      parse_stream(in_.get(), [this](const input_event_t& event) { push(event); }, err_.get());
      push(input_event_t{shutdown_t{}});
    }

  private:
    void push(const input_event_t& event) noexcept {
      while (!token_.stop_requested() && !queue_->try_push(event)) {
        runtime::cpu_pause();
      }
    }

    std::reference_wrapper<std::istream> in_;
    std::reference_wrapper<std::ostream> err_;
    std::shared_ptr<order_queue_type> queue_;
    std::stop_token token_;
  };

  /// Applies events to @ref clob_t; forwards @ref shutdown_t downstream and ends the loop (returns true).
  template <std::size_t Capacity>
  class matcher_handler_t {
  public:
    using queue_emitter_type = queue_emitter_t<Capacity>;
    using book_type = clob_t<queue_emitter_type>;
    using output_queue_type = runtime::spsc_queue_t<output_event_t, Capacity>;

    matcher_handler_t(book_type book, std::shared_ptr<output_queue_type> out, std::stop_token token) noexcept
      : book_(std::move(book)), out_(std::move(out)), token_(std::move(token)) {}

    [[nodiscard]] bool operator()(const input_event_t& event) noexcept {
      if (std::holds_alternative<shutdown_t>(event)) {
        forward_shutdown();
        return true;
      }
      book_(event);
      return false;
    }

  private:
    void forward_shutdown() noexcept {
      const output_event_t sentinel{shutdown_t{}};
      while (!token_.stop_requested() && !out_->try_push(sentinel)) {
        runtime::cpu_pause();
      }
    }

    book_type book_;
    std::shared_ptr<output_queue_type> out_;
    std::stop_token token_;
  };

  /// Formats @ref output_event_t lines; @ref shutdown_t triggers flush and loop exit (handler returns true).
  class writer_handler_t {
  public:
    explicit writer_handler_t(output_formatter_t& formatter) noexcept : formatter_(formatter) {}

    [[nodiscard]] bool operator()(const output_event_t& event) noexcept {
      bool done = false;
      std::visit(
        [this, &done](const auto& concrete) {
          using event_type = std::decay_t<decltype(concrete)>;
          if constexpr (std::is_same_v<event_type, shutdown_t>) {
            formatter_.get().sink().flush();
            done = true;
          } else {
            formatter_.get()(concrete);
          }
        },
        event
      );
      return done;
    }

  private:
    std::reference_wrapper<output_formatter_t> formatter_;
  };

}  // namespace matching
