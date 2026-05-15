#include <gtest/gtest.h>

#include <chrono>
#include <memory>
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
#include "matching/stream_pipeline.hpp"

namespace matching {

  namespace {

    /// In-memory @c std::ostringstream sinks so we can compare the writer agent's output stream
    /// byte-for-byte with the golden expected output (`res/sample_output.txt`).
    using memory_sink_t = std::ostringstream;
    constexpr std::size_t queue_capacity_v = 1u << 14;
    using order_queue_t = runtime::spsc_queue_t<input_event_t, queue_capacity_v>;
    using output_queue_t = runtime::spsc_queue_t<output_event_t, queue_capacity_v>;

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

    const auto order_queue = std::make_shared<order_queue_t>();
    const auto output_queue = std::make_shared<output_queue_t>();

    std::stop_source source;
    const std::stop_token token = source.get_token();

    output_formatter_t formatter{out};
    clob_factory_t<queue_emitter_t<queue_capacity_v>> factory{
      1024,
      queue_emitter_t<queue_capacity_v>{output_queue, token},
    };
    auto book = std::move(factory).create();

    auto reader_loop = reader_loop_t<queue_capacity_v>{in, err, order_queue, token};
    auto reader_agent = runtime::make_agent(std::move(reader_loop), std::nullopt);

    auto matcher_loop = runtime::make_event_loop(
      runtime::queue_source_shared_t<order_queue_t>{order_queue},
      matcher_handler_t<queue_capacity_v>{std::move(book), output_queue, token},
      token
    );
    auto matcher_agent = runtime::make_agent(std::move(matcher_loop), std::nullopt);

    auto writer_loop = runtime::make_event_loop(
      runtime::queue_source_shared_t<output_queue_t>{output_queue},
      writer_handler_t{formatter},
      token
    );
    auto writer_agent = runtime::make_agent(std::move(writer_loop), std::nullopt);

    auto system = runtime::make_agent_system(source, std::move(reader_agent), std::move(matcher_agent), std::move(writer_agent));
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
    const auto order_queue = std::make_shared<order_queue_t>();
    const auto output_queue = std::make_shared<output_queue_t>();

    std::stop_source source;
    const std::stop_token token = source.get_token();

    memory_sink_t out;
    output_formatter_t formatter{out};
    clob_factory_t<queue_emitter_t<queue_capacity_v>> factory{
      1024,
      queue_emitter_t<queue_capacity_v>{output_queue, token},
    };
    auto book = std::move(factory).create();

    auto matcher_loop = runtime::make_event_loop(
      runtime::queue_source_shared_t<order_queue_t>{order_queue},
      matcher_handler_t<queue_capacity_v>{std::move(book), output_queue, token},
      token
    );
    auto matcher_agent = runtime::make_agent(std::move(matcher_loop), std::nullopt);

    auto writer_loop = runtime::make_event_loop(
      runtime::queue_source_shared_t<output_queue_t>{output_queue},
      writer_handler_t{formatter},
      token
    );
    auto writer_agent = runtime::make_agent(std::move(writer_loop), std::nullopt);

    auto system = runtime::make_agent_system(source, std::move(matcher_agent), std::move(writer_agent));
    system.start();

    const auto t0 = std::chrono::steady_clock::now();
    source.request_stop();
    system.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_LT(elapsed, std::chrono::seconds{2});
  }

}  // namespace matching
