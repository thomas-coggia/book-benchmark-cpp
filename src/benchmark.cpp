#include "matching/benchmark/benchmark.hpp"

#include <iostream>
#include <print>
#include <string>

#include "app/benchmark_config.hpp"
#include "matching/clob_factory.hpp"
#include "matching/order_book.hpp"

namespace {

  using namespace matching;
  using namespace matching::benchmark;

  constexpr std::size_t default_book_capacity = 4'000'000;

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto config = benchmark_config::parse_config(argc, argv);

    const benchmark_config_t& bench = config.benchmark;

    std::println(
      std::cout,
      "matching::benchmark — {} preset(s), iterations = {}, warmup = {}, latency = {}",
      config.profiles.size(),
      bench.iterations,
      bench.warmup_events,
      bench.collect_latency ? "on" : "off"
    );
    std::println(
      std::cout,
      "  pinning: producer={} matcher={} stats={}",
      bench.producer_cpu.has_value() ? std::to_string(*bench.producer_cpu) : "off",
      bench.matcher_cpu.has_value() ? std::to_string(*bench.matcher_cpu) : "off",
      bench.stats_cpu.has_value() ? std::to_string(*bench.stats_cpu) : "off"
    );

    for (const auto& profile : config.profiles) {
      auto factory = [&](counting_emitter_t emitter) {
        return clob_factory_t{default_book_capacity, std::move(emitter)};
      };
      const auto result = run_preset(profile, bench, factory);
      print_preset_result(std::cout, result);
      std::cout.flush();
    }
  } catch (const benchmark_config::help_requested&) {
    return 0;
  } catch (const benchmark_config::parse_error&) {
    return 1;
  }

  return 0;
}
