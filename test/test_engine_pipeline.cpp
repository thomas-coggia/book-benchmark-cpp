#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "matching/clob_factory.hpp"
#include "matching/input_event.hpp"
#include "matching/output_event.hpp"
#include "matching/output_formatter.hpp"
#include "matching/runtime/agent.hpp"
#include "matching/runtime/agent_system.hpp"
#include "matching/runtime/event_loop.hpp"
#include "matching/runtime/spsc_queue.hpp"
#include "matching/stream_pipeline.hpp"
#include "matching_test_res_dir.hpp"

namespace matching {

  namespace {

    using memory_sink_t = std::ostringstream;
    constexpr std::size_t queue_capacity_v = 1u << 14;
    using order_queue_t = runtime::spsc_queue_t<input_event_t, queue_capacity_v>;
    using output_queue_t = runtime::spsc_queue_t<output_event_t, queue_capacity_v>;

    [[nodiscard]] std::string read_entire_file(const std::filesystem::path& path) {
      std::ifstream in(path);
      std::ostringstream buf;
      if (in) {
        buf << in.rdbuf();
      }
      return std::move(buf).str();
    }

    /// Drives the same three-agent pipeline as @c matching_engine until join.
    void
    run_matching_engine_on_input(const std::string& input_text, std::string& stdout_text, std::string& stderr_text) {
      std::istringstream in{input_text};

      memory_sink_t out{};
      memory_sink_t err{};

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
        runtime::queue_source_shared_t<output_queue_t>{output_queue}, writer_handler_t{formatter}, token
      );
      auto writer_agent = runtime::make_agent(std::move(writer_loop), std::nullopt);

      auto system =
        runtime::make_agent_system(source, std::move(reader_agent), std::move(matcher_agent), std::move(writer_agent));
      system.start();
      system.join();

      stdout_text = out.str();
      stderr_text = err.str();
    }

  }  // namespace

  TEST(EnginePipelineTest, GoldenRecordedStdoutMatchesSampleFiles) {
    const std::filesystem::path root = matching_test_res_dir;

    for (const int ix : {1, 2, 3, 4, 5}) {
      const std::filesystem::path in_path = root / ("sample_" + std::to_string(ix) + ".input.txt");
      const std::filesystem::path out_path = root / ("sample_" + std::to_string(ix) + ".output.txt");
      const std::filesystem::path err_path = root / ("sample_" + std::to_string(ix) + ".stderr.txt");

      ASSERT_TRUE(std::filesystem::exists(in_path)) << "missing input " << in_path.string();
      ASSERT_TRUE(std::filesystem::exists(out_path)) << "missing golden " << out_path.string();

      const std::string input = read_entire_file(in_path);
      const std::string expected_stdout = read_entire_file(out_path);
      const bool has_stderr_golden = std::filesystem::exists(err_path);
      const std::string expected_stderr = has_stderr_golden ? read_entire_file(err_path) : std::string{};

      std::string got_stdout;
      std::string got_stderr;
      run_matching_engine_on_input(input, got_stdout, got_stderr);

      EXPECT_EQ(got_stdout, expected_stdout) << "stdout mismatch for scenario " << ix;
      if (has_stderr_golden) {
        EXPECT_EQ(got_stderr, expected_stderr) << "stderr mismatch for scenario " << ix;
      } else {
        EXPECT_TRUE(got_stderr.empty()) << "unexpected stderr for scenario " << ix << ":\n" << got_stderr;
      }
    }
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
      runtime::queue_source_shared_t<output_queue_t>{output_queue}, writer_handler_t{formatter}, token
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
