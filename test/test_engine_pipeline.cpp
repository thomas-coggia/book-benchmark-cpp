#include <gtest/gtest.h>

#include <chrono>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "matching/clob_factory.hpp"
#include "matching/input_event.hpp"
#include "matching/input_parser.hpp"
#include "matching/order_book.hpp"
#include "matching/output_event.hpp"
#include "matching/output_formatter.hpp"
#include "matching/runtime/agent.hpp"
#include "matching/runtime/agent_system.hpp"
#include "matching/runtime/event_loop.hpp"
#include "matching/runtime/spsc_queue.hpp"

namespace matching {

  namespace {

    /// In-memory @c std::ostringstream sinks so we can compare the writer agent's output stream
    /// byte-for-byte with the golden expected output (`res/sample_output.txt`).
    using memory_sink_t = std::ostringstream;
    constexpr std::size_t queue_capacity_v = 1u << 14;
    using order_queue_t = runtime::spsc_queue_t<input_event_t, queue_capacity_v>;
    using output_queue_t = runtime::spsc_queue_t<output_event_t, queue_capacity_v>;

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
          runtime::cpu_pause();
        }
      }

      output_queue_t* queue_{nullptr};
      std::stop_token token_;
    };

    class reader_loop_t {
    public:
      reader_loop_t(std::istream& in, std::ostream& err, order_queue_t& queue, std::stop_token token) noexcept
        : in_(&in), err_(&err), queue_(&queue), token_(std::move(token)) {}

      void run() {
        parse_stream(*in_, [this](const input_event_t& event) {
          push(event);
        }, *err_);
        push(input_event_t{shutdown_t{}});
      }

    private:
      void push(const input_event_t& event) noexcept {
        while (!token_.stop_requested() && !queue_->try_push(event)) {
          runtime::cpu_pause();
        }
      }

      std::istream* in_{nullptr};
      std::ostream* err_{nullptr};
      order_queue_t* queue_{nullptr};
      std::stop_token token_;
    };

    using book_t = clob_t<queue_emitter_t>;

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
          runtime::cpu_pause();
        }
      }

      book_t* book_{nullptr};
      output_queue_t* out_{nullptr};
      std::stop_token token_;
    };

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

  TEST(EnginePipelineTest, BundledSampleReproducesByteForByte) {
    const std::string input =
      "0,1000000,1,1,1075\n"
      "0,1000001,0,9,1000\n"
      "0,1000002,0,30,975\n"
      "0,1000003,1,10,1050\n"
      "0,1000004,0,10,950\n"
      "BADMESSAGE\n"
      "0,1000005,1,2,1025\n"
      "0,1000006,0,1,1000\n"
      "1,1000004\n"
      "0,1000007,1,5,1025\n"
      "0,1000008,0,3,1050\n";

    std::istringstream in{input};

    memory_sink_t out;
    memory_sink_t err;

    order_queue_t order_queue;
    output_queue_t output_queue;

    std::stop_source source;
    const std::stop_token token = source.get_token();

    output_formatter_t formatter{out};
    clob_factory_t<queue_emitter_t> factory{1024, queue_emitter_t{output_queue, token}};
    book_t book = std::move(factory).create();

    reader_loop_t reader_loop{in, err, order_queue, token};
    runtime::agent_t reader_agent{std::move(reader_loop), std::nullopt};

    using matcher_loop_t = runtime::event_loop_t<runtime::queue_source_t<order_queue_t>, matcher_handler_t>;
    matcher_loop_t matcher_loop{
      runtime::queue_source_t<order_queue_t>{order_queue},
      matcher_handler_t{book, output_queue, token},
      token,
    };
    runtime::agent_t matcher_agent{std::move(matcher_loop), std::nullopt};

    using writer_loop_t = runtime::event_loop_t<runtime::queue_source_t<output_queue_t>, writer_handler_t>;
    writer_loop_t writer_loop{
      runtime::queue_source_t<output_queue_t>{output_queue},
      writer_handler_t{formatter},
      token,
    };
    runtime::agent_t writer_agent{std::move(writer_loop), std::nullopt};

    runtime::agent_system_t system{source, std::move(reader_agent), std::move(matcher_agent), std::move(writer_agent)};
    system.start();
    system.join();

    const std::string expected =
      "2,2,1025\n"
      "4,1000008,1\n"
      "3,1000005\n"
      "2,1,1025\n"
      "3,1000008\n"
      "4,1000007,4\n";

    EXPECT_EQ(out.str(), expected);
    EXPECT_NE(err.str().find("Unknown message type: BADMESSAGE"), std::string::npos);
  }

  TEST(EnginePipelineTest, AbortMidStreamJoinsCleanly) {
    // No input source: the reader will block on stdin (it never gets one), but the
    // production binary always feeds a real source. Here we exercise just the matcher and
    // writer with no events queued, then trigger an abort and assert the system joins
    // promptly (no hung loops).
    order_queue_t order_queue;
    output_queue_t output_queue;

    std::stop_source source;
    const std::stop_token token = source.get_token();

    memory_sink_t out;
    output_formatter_t formatter{out};
    clob_factory_t<queue_emitter_t> factory{1024, queue_emitter_t{output_queue, token}};
    book_t book = std::move(factory).create();

    using matcher_loop_t = runtime::event_loop_t<runtime::queue_source_t<order_queue_t>, matcher_handler_t>;
    matcher_loop_t matcher_loop{
      runtime::queue_source_t<order_queue_t>{order_queue},
      matcher_handler_t{book, output_queue, token},
      token,
    };
    runtime::agent_t matcher_agent{std::move(matcher_loop), std::nullopt};

    using writer_loop_t = runtime::event_loop_t<runtime::queue_source_t<output_queue_t>, writer_handler_t>;
    writer_loop_t writer_loop{
      runtime::queue_source_t<output_queue_t>{output_queue},
      writer_handler_t{formatter},
      token,
    };
    runtime::agent_t writer_agent{std::move(writer_loop), std::nullopt};

    runtime::agent_system_t system{source, std::move(matcher_agent), std::move(writer_agent)};
    system.start();

    const auto t0 = std::chrono::steady_clock::now();
    source.request_stop();
    system.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_LT(elapsed, std::chrono::seconds{2});
  }

}  // namespace matching
