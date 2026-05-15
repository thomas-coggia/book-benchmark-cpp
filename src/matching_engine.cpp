#include <cstddef>
#include <iostream>
#include <memory>
#include <print>
#include <stop_token>
#include <utility>

#include "matching/clob_factory.hpp"
#include "matching/runtime/agent.hpp"
#include "matching/runtime/agent_system.hpp"
#include "matching/runtime/event_loop.hpp"
#include "matching/runtime/signal_handler.hpp"
#include "matching/runtime/spsc_queue.hpp"
#include "matching/stream_pipeline.hpp"
#include "app/matching_engine_config.hpp"

int main(int argc, char** argv) {
  try {
    const auto config = matching_engine_config::parse_config(argc, argv);

    using namespace matching;
    using namespace matching::runtime;

    /// SPSC ring depth for both queues: large enough that a brief context switch on the
    /// writer does not stall the matcher.
    constexpr std::size_t queue_capacity_v = 1u << 16;
    using order_queue_t = spsc_queue_t<input_event_t, queue_capacity_v>;
    using output_queue_t = spsc_queue_t<output_event_t, queue_capacity_v>;

    const auto order_queue = std::make_shared<order_queue_t>();
    const auto output_queue = std::make_shared<output_queue_t>();

    install_signal_handler();
    std::stop_token token = global_stop_source().get_token();

    output_formatter_t formatter{std::cout};
    clob_factory_t<queue_emitter_t<queue_capacity_v>> factory{
      config.capacity,
      queue_emitter_t<queue_capacity_v>{output_queue, token},
    };
    auto book = std::move(factory).create();

    auto reader_loop = reader_loop_t<queue_capacity_v>{std::cin, std::cerr, order_queue, token};
    auto reader_agent = make_agent(std::move(reader_loop), config.reader_cpu);

    auto matcher_loop = make_event_loop(
      queue_source_shared_t<order_queue_t>{order_queue},
      matcher_handler_t<queue_capacity_v>{std::move(book), output_queue, token},
      token
    );
    auto matcher_agent = make_agent(std::move(matcher_loop), config.matcher_cpu);

    auto writer_loop = make_event_loop(
      queue_source_shared_t<output_queue_t>{output_queue},
      writer_handler_t{formatter},
      token
    );
    auto writer_agent = make_agent(std::move(writer_loop), config.writer_cpu);

    auto system = make_agent_system(global_stop_source(), std::move(reader_agent), std::move(matcher_agent), std::move(writer_agent));
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
