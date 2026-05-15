#include <cstddef>
#include <iostream>
#include <print>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>

#include "matching/clob_factory.hpp"
#include "matching/input_parser.hpp"
#include "matching/output_formatter.hpp"
#include "matching/runtime/agent.hpp"
#include "matching/runtime/agent_system.hpp"
#include "matching/runtime/event_loop.hpp"
#include "matching/runtime/signal_handler.hpp"
#include "matching/runtime/spsc_queue.hpp"
#include "app/matching_engine_config.hpp"

namespace {

  using namespace matching;
  using namespace matching::runtime;

  /// Default SPSC ring depth. 65,536 is enough that a single context switch on the writer
  /// agent does not stall the matcher; both queues use the same value for symmetry.
  constexpr std::size_t default_queue_capacity_v = 1u << 16;

  using order_queue_t = spsc_queue_t<input_event_t, default_queue_capacity_v>;
  using output_queue_t = spsc_queue_t<output_event_t, default_queue_capacity_v>;

  /// Emitter the @ref clob_t writes through. Wraps each trade / fill in @ref output_event_t
  /// and pushes it onto the matcher → writer queue. The push spin is stop-aware so an abort
  /// (via the global stop_source) does not deadlock the matcher when the writer has died.
  class queue_emitter_t {
  public:
    queue_emitter_t(output_queue_t& queue, std::stop_token token) noexcept
      : queue_(&queue), token_(std::move(token)) {}

    void operator()(const trade_event_t& event) noexcept {
      push(output_event_t{event});
    }
    void operator()(const order_fully_filled_t& event) noexcept {
      push(output_event_t{event});
    }
    void operator()(const order_partially_filled_t& event) noexcept {
      push(output_event_t{event});
    }

  private:
    void push(const output_event_t& event) noexcept {
      while (!token_.stop_requested() && !queue_->try_push(event)) {
        cpu_pause();
      }
    }

    output_queue_t* queue_{nullptr};
    std::stop_token token_;
  };

  using book_t = clob_t<queue_emitter_t>;

  /// Reader loop: pulls newline-terminated lines from @c std::cin via the existing
  /// @ref parse_stream, pushes each parsed event onto the order queue, and finishes the
  /// stream by pushing an in-band @ref matching::shutdown_t before returning. Parse errors continue
  /// to be reported to @c std::cerr by @ref parse_stream itself.
  class reader_loop_t {
  public:
    reader_loop_t(std::istream& in, std::ostream& err, order_queue_t& queue, std::stop_token token) noexcept
      : in_(&in), err_(&err), queue_(&queue), token_(std::move(token)) {}

    void run() {
      parse_stream(*in_, [this](const input_event_t& event) {
        push(event);
      }, *err_);
      // EOF reached: emit shutdown so the matcher can drain and forward it downstream.
      push(input_event_t{shutdown_t{}});
    }

  private:
    void push(const input_event_t& event) noexcept {
      while (!token_.stop_requested() && !queue_->try_push(event)) {
        cpu_pause();
      }
    }

    std::istream* in_{nullptr};
    std::ostream* err_{nullptr};
    order_queue_t* queue_{nullptr};
    std::stop_token token_;
  };

  /// Matcher handler used by the generic @c event_loop_t. Forwards real events into the
  /// CLOB; on @ref matching::shutdown_t it relays the same sentinel downstream and returns
  /// @c true so the loop terminates.
  class matcher_handler_t {
  public:
    matcher_handler_t(book_t& book, output_queue_t& out, std::stop_token token) noexcept
      : book_(&book), out_(&out), token_(std::move(token)) {}

    [[nodiscard]] bool operator()(const input_event_t& event) noexcept {
      if (std::holds_alternative<shutdown_t>(event)) {
        forward_shutdown();
        return true;
      }
      (*book_)(event);
      return false;
    }

  private:
    void forward_shutdown() noexcept {
      const output_event_t sentinel{shutdown_t{}};
      while (!token_.stop_requested() && !out_->try_push(sentinel)) {
        cpu_pause();
      }
    }

    book_t* book_{nullptr};
    output_queue_t* out_{nullptr};
    std::stop_token token_;
  };

  /// Writer handler. Visits the variant: real output events go through the formatter, @ref
  /// matching::shutdown_t triggers a final @c flush on the formatter sink and terminates the loop.
  class writer_handler_t {
  public:
    explicit writer_handler_t(output_formatter_t& formatter) noexcept
      : formatter_(&formatter) {}

    [[nodiscard]] bool operator()(const output_event_t& event) noexcept {
      bool done = false;
      std::visit([this, &done](const auto& concrete) {
        using event_type = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<event_type, shutdown_t>) {
          formatter_->sink().flush();
          done = true;
        } else {
          (*formatter_)(concrete);
        }
      }, event);
      return done;
    }

  private:
    output_formatter_t* formatter_{nullptr};
  };

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto config = matching_engine_config::parse_config(argc, argv);

    order_queue_t order_queue;
    output_queue_t output_queue;

    install_signal_handler();
    std::stop_source& source = global_stop_source();
    std::stop_token token = source.get_token();

    output_formatter_t formatter{std::cout};
    clob_factory_t<queue_emitter_t> factory{config.capacity, queue_emitter_t{output_queue, token}};
    book_t book = std::move(factory).create();

    reader_loop_t reader_loop{std::cin, std::cerr, order_queue, token};
    agent_t reader_agent{std::move(reader_loop), config.reader_cpu};

    using matcher_loop_t = event_loop_t<queue_source_t<order_queue_t>, matcher_handler_t>;
    matcher_loop_t matcher_loop{
      queue_source_t<order_queue_t>{order_queue},
      matcher_handler_t{book, output_queue, token},
      token,
    };
    agent_t matcher_agent{std::move(matcher_loop), config.matcher_cpu};

    using writer_loop_t = event_loop_t<queue_source_t<output_queue_t>, writer_handler_t>;
    writer_loop_t writer_loop{
      queue_source_t<output_queue_t>{output_queue},
      writer_handler_t{formatter},
      token,
    };
    agent_t writer_agent{std::move(writer_loop), config.writer_cpu};

    agent_system_t system{source, std::move(reader_agent), std::move(matcher_agent), std::move(writer_agent)};
    system.start();
    system.join();

    uninstall_signal_handler();
  } catch (const matching_engine_config::help_requested&) {
    return 0;
  } catch (const matching_engine_config::parse_error&) {
    return 1;
  }

  return 0;
}
