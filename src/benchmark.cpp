#include <array>
#include <cstdint>
#include <cxxopts.hpp>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "matching/benchmark/benchmark.hpp"
#include "matching/benchmark/market_profile.hpp"
#include "matching/clob_factory.hpp"
#include "matching/order_book.hpp"

namespace {

  using namespace matching;
  using namespace matching::benchmark;

  /// Capacity for the SoA storage during a benchmark run. Must comfortably exceed the
  /// per-iteration order count plus warmup, since both feed the same factory-built book.
  constexpr std::size_t default_book_capacity = 4'000'000;

  /// Schema of the benchmark CLI. Held as a single table so @c --help renders it directly.
  struct flag_doc_t {
    std::string_view flag;
    std::string_view help;
  };

  inline constexpr std::array<flag_doc_t, 20> flag_docs = {{
    {"--profile <name>", "quiet | active | cancel | volatile | sweep | all (default: all)"},
    {"--orders <N>", "override num_orders for the run (default: preset value)"},
    {"--warmup <N>", "warmup events before timing (default: 10000)"},
    {"--iterations <N>", "number of timed runs to aggregate (default: 5)"},
    {"--seed <N>", "RNG seed (default: 42)"},
    {"--latency-stats {on|off}", "enable per-event latency sampling (default: on)"},
    {"--producer-cpu <N>", "pin the producer agent to CPU N (omit to leave unpinned)"},
    {"--matcher-cpu <N>", "pin the matcher agent to CPU N (omit to leave unpinned)"},
    {"--stats-cpu <N>", "pin the stats agent to CPU N (omit to leave unpinned)"},
    {"--cancel-ratio <F>", "override P(event = Cancel | live set non-empty)"},
    {"--aggressive-ratio <F>", "override P(Add crosses opposite touch)"},
    {"--buy-bias <F>", "override P(side = Buy)"},
    {"--mu <F>", "override mid log-drift per step"},
    {"--sigma <F>", "override mid log-volatility per step"},
    {"--initial-mid <N>", "override starting mid in ticks"},
    {"--tick-size <N>", "override tick size in price units"},
    {"--place-decay <F>", "override exponential decay for distance-from-touch on passive adds"},
    {"--qty-log-mean <F>", "override log-normal underlying mean for quantity"},
    {"--qty-log-stddev <F>", "override log-normal underlying stddev for quantity"},
    {"--qty-min/--qty-max <N>", "override clamp bounds for quantity"},
  }};

  void print_help(const char* program_name) {
    std::println(std::cout, "Usage: {} [flags]", program_name);
    std::println(std::cout, "");
    std::println(std::cout, "Runs the matching engine through one or all five synthetic-market presets.");
    std::println(std::cout, "");
    std::println(std::cout, "Flags:");
    for (const auto& [flag, help] : flag_docs) {
      std::println(std::cout, "  {:<28} {}", flag, help);
    }
    std::println(std::cout, "  --help, -h                   print this message and exit");
  }

  template <typename T, typename Setter>
  void apply_override(const cxxopts::ParseResult& result, const char* flag, Setter&& setter) {
    if (result.count(flag) == 0) {
      return;
    }
    try {
      setter(result[flag].as<T>());
    } catch (const cxxopts::exceptions::exception& ex) {
      std::println(std::cerr, "benchmark: invalid value for --{}: {}", flag, ex.what());
      std::exit(1);
    }
  }

  market_profile_t apply_overrides(market_profile_t base, const cxxopts::ParseResult& result) {
    apply_override<std::size_t>(result, "orders", [&](std::size_t v) { base.num_orders = v; });
    apply_override<std::uint64_t>(result, "seed", [&](std::uint64_t v) { base.seed = v; });
    apply_override<double>(result, "cancel-ratio", [&](double v) { base.cancel_ratio = v; });
    apply_override<double>(result, "aggressive-ratio", [&](double v) { base.aggressive_ratio = v; });
    apply_override<double>(result, "buy-bias", [&](double v) { base.buy_bias = v; });
    apply_override<double>(result, "mu", [&](double v) { base.mu = v; });
    apply_override<double>(result, "sigma", [&](double v) { base.sigma = v; });
    apply_override<std::int32_t>(result, "initial-mid", [&](std::int32_t v) { base.initial_mid = v; });
    apply_override<std::int32_t>(result, "tick-size", [&](std::int32_t v) { base.tick_size = v; });
    apply_override<double>(result, "place-decay", [&](double v) { base.place_decay = v; });
    apply_override<double>(result, "qty-log-mean", [&](double v) { base.qty_log_mean = v; });
    apply_override<double>(result, "qty-log-stddev", [&](double v) { base.qty_log_stddev = v; });
    apply_override<std::int32_t>(result, "qty-min", [&](std::int32_t v) { base.qty_min = v; });
    apply_override<std::int32_t>(result, "qty-max", [&](std::int32_t v) { base.qty_max = v; });
    return base;
  }

  void apply_cpu_override(const cxxopts::ParseResult& result, const char* flag, std::optional<int>& slot) {
    if (result.count(flag) == 0) {
      return;
    }
    try {
      const int v = result[flag].as<int>();
      if (v < 0) {
        std::println(std::cerr, "benchmark: invalid value for --{}: '{}' (expected non-negative integer)", flag, v);
        std::exit(1);
      }
      slot = v;
    } catch (const cxxopts::exceptions::exception& ex) {
      std::println(std::cerr, "benchmark: invalid value for --{}: {}", flag, ex.what());
      std::exit(1);
    }
  }

  benchmark_config_t resolve_runtime(const cxxopts::ParseResult& result) {
    benchmark_config_t cfg{};
    apply_override<std::size_t>(result, "warmup", [&](std::size_t v) { cfg.warmup_events = v; });
    apply_override<std::size_t>(result, "iterations", [&](std::size_t v) { cfg.iterations = v; });
    apply_cpu_override(result, "producer-cpu", cfg.producer_cpu);
    apply_cpu_override(result, "matcher-cpu", cfg.matcher_cpu);
    apply_cpu_override(result, "stats-cpu", cfg.stats_cpu);
    if (result.count("latency-stats") != 0) {
      std::string v;
      try {
        v = result["latency-stats"].as<std::string>();
      } catch (const cxxopts::exceptions::exception& ex) {
        std::println(std::cerr, "benchmark: invalid value for --latency-stats: {}", ex.what());
        std::exit(1);
      }
      if (v == "on" || v == "true" || v == "1") {
        cfg.collect_latency = true;
      } else if (v == "off" || v == "false" || v == "0") {
        cfg.collect_latency = false;
      } else {
        std::println(std::cerr, "benchmark: --latency-stats must be on|off, got '{}'", v);
        std::exit(1);
      }
    }
    return cfg;
  }

  std::vector<market_profile_t> select_presets(const cxxopts::ParseResult& result) {
    static constexpr std::array<const market_profile_t*, 5> all = {
      &profile_quiet_build,
      &profile_active_match,
      &profile_cancel_heavy,
      &profile_volatile,
      &profile_sweep,
    };

    std::string pick = "all";
    if (result.count("profile") != 0) {
      pick = result["profile"].as<std::string>();
    }
    std::vector<market_profile_t> out;
    if (pick == "all") {
      for (const auto* p : all) {
        out.push_back(*p);
      }
      return out;
    }
    for (const auto* p : all) {
      if (p->name == pick) {
        out.push_back(*p);
        return out;
      }
    }
    std::println(std::cerr, "benchmark: unknown --profile '{}'. Valid: quiet | active | cancel | volatile | sweep | all", pick);
    std::exit(1);
  }

  cxxopts::Options build_options(const char* program_name) {
    cxxopts::Options opts(program_name, "");
    opts.add_options()
      ("h,help", "Print help")
      ("profile", "Preset name", cxxopts::value<std::string>())
      ("orders", "Order count override", cxxopts::value<std::size_t>())
      ("warmup", "Warmup events", cxxopts::value<std::size_t>())
      ("iterations", "Timed iterations", cxxopts::value<std::size_t>())
      ("seed", "RNG seed", cxxopts::value<std::uint64_t>())
      ("latency-stats", "on|off", cxxopts::value<std::string>())
      ("producer-cpu", "Producer CPU", cxxopts::value<int>())
      ("matcher-cpu", "Matcher CPU", cxxopts::value<int>())
      ("stats-cpu", "Stats CPU", cxxopts::value<int>())
      ("cancel-ratio", "Cancel probability", cxxopts::value<double>())
      ("aggressive-ratio", "Aggressive add probability", cxxopts::value<double>())
      ("buy-bias", "Buy-side probability", cxxopts::value<double>())
      ("mu", "Mid log-drift", cxxopts::value<double>())
      ("sigma", "Mid log-volatility", cxxopts::value<double>())
      ("initial-mid", "Starting mid (ticks)", cxxopts::value<std::int32_t>())
      ("tick-size", "Tick size", cxxopts::value<std::int32_t>())
      ("place-decay", "Passive distance decay", cxxopts::value<double>())
      ("qty-log-mean", "Qty log-normal mean", cxxopts::value<double>())
      ("qty-log-stddev", "Qty log-normal stddev", cxxopts::value<double>())
      ("qty-min", "Quantity minimum", cxxopts::value<std::int32_t>())
      ("qty-max", "Quantity maximum", cxxopts::value<std::int32_t>());
    return opts;
  }

}  // namespace

int main(int argc, char** argv) {
  const char* prog = argv != nullptr && argv[0] != nullptr ? argv[0] : "benchmark";
  cxxopts::Options options = build_options(prog);

  cxxopts::ParseResult parsed;
  try {
    parsed = options.parse(argc, argv);
  } catch (const cxxopts::exceptions::exception& ex) {
    std::println(std::cerr, "benchmark: {}", ex.what());
    return 1;
  }

  if (parsed.count("help") != 0) {
    print_help(prog);
    return 0;
  }

  const auto presets = select_presets(parsed);
  const auto runtime = resolve_runtime(parsed);

  std::println(std::cout, "matching::benchmark — {} preset(s), iterations = {}, warmup = {}, latency = {}",
    presets.size(), runtime.iterations, runtime.warmup_events, runtime.collect_latency ? "on" : "off");
  std::println(std::cout, "  pinning: producer={} matcher={} stats={}",
    runtime.producer_cpu.has_value() ? std::to_string(*runtime.producer_cpu) : "off",
    runtime.matcher_cpu.has_value() ? std::to_string(*runtime.matcher_cpu) : "off",
    runtime.stats_cpu.has_value() ? std::to_string(*runtime.stats_cpu) : "off");

  for (const auto& base : presets) {
    const market_profile_t profile = apply_overrides(base, parsed);
    auto factory = [&](counting_emitter_t emitter) {
      return clob_factory_t{default_book_capacity, std::move(emitter)};
    };
    const auto result = run_preset(profile, runtime, factory);
    print_preset_result(std::cout, result);
    std::cout.flush();
  }

  return 0;
}
