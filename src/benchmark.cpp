#include <array>
#include <charconv>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "matching/benchmark/benchmark.hpp"
#include "matching/benchmark/market_profile.hpp"
#include "matching/cli.hpp"
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

  template <typename T>
  bool parse_into(std::string_view sv, T& out) noexcept {
    if (sv.empty()) {
      return false;
    }
    if constexpr (std::is_floating_point_v<T>) {
      try {
        std::size_t consumed = 0;
        const std::string buf{sv};
        const double parsed = std::stod(buf, &consumed);
        if (consumed != buf.size()) {
          return false;
        }
        out = static_cast<T>(parsed);
        return true;
      } catch (...) {
        return false;
      }
    } else {
      const auto* const begin = sv.data();
      const auto* const end = sv.data() + sv.size();
      const auto [ptr, ec] = std::from_chars(begin, end, out);
      return ec == std::errc{} && ptr == end;
    }
  }

  template <typename T, typename Setter>
  void apply_override(const cli_args_t& args, std::string_view flag, Setter&& setter) {
    const auto raw = args.get(flag);
    if (!raw.has_value()) {
      return;
    }
    T value{};
    if (!parse_into<T>(*raw, value)) {
      std::println(std::cerr, "benchmark: invalid value for --{}: '{}'", flag, *raw);
      std::exit(1);
    }
    setter(value);
  }

  market_profile_t apply_overrides(market_profile_t base, const cli_args_t& args) {
    apply_override<std::size_t>(args, "orders", [&](std::size_t v) { base.num_orders = v; });
    apply_override<std::uint64_t>(args, "seed", [&](std::uint64_t v) { base.seed = v; });
    apply_override<double>(args, "cancel-ratio", [&](double v) { base.cancel_ratio = v; });
    apply_override<double>(args, "aggressive-ratio", [&](double v) { base.aggressive_ratio = v; });
    apply_override<double>(args, "buy-bias", [&](double v) { base.buy_bias = v; });
    apply_override<double>(args, "mu", [&](double v) { base.mu = v; });
    apply_override<double>(args, "sigma", [&](double v) { base.sigma = v; });
    apply_override<std::int32_t>(args, "initial-mid", [&](std::int32_t v) { base.initial_mid = v; });
    apply_override<std::int32_t>(args, "tick-size", [&](std::int32_t v) { base.tick_size = v; });
    apply_override<double>(args, "place-decay", [&](double v) { base.place_decay = v; });
    apply_override<double>(args, "qty-log-mean", [&](double v) { base.qty_log_mean = v; });
    apply_override<double>(args, "qty-log-stddev", [&](double v) { base.qty_log_stddev = v; });
    apply_override<std::int32_t>(args, "qty-min", [&](std::int32_t v) { base.qty_min = v; });
    apply_override<std::int32_t>(args, "qty-max", [&](std::int32_t v) { base.qty_max = v; });
    return base;
  }

  void apply_cpu_override(const cli_args_t& args, std::string_view flag, std::optional<int>& slot) {
    if (const auto raw = args.get(flag); raw.has_value()) {
      int v{};
      if (!parse_into<int>(*raw, v) || v < 0) {
        std::println(std::cerr, "benchmark: invalid value for --{}: '{}' (expected non-negative integer)", flag, *raw);
        std::exit(1);
      }
      slot = v;
    }
  }

  benchmark_config_t resolve_runtime(const cli_args_t& args) {
    benchmark_config_t cfg{};
    apply_override<std::size_t>(args, "warmup", [&](std::size_t v) { cfg.warmup_events = v; });
    apply_override<std::size_t>(args, "iterations", [&](std::size_t v) { cfg.iterations = v; });
    apply_cpu_override(args, "producer-cpu", cfg.producer_cpu);
    apply_cpu_override(args, "matcher-cpu", cfg.matcher_cpu);
    apply_cpu_override(args, "stats-cpu", cfg.stats_cpu);
    if (const auto v = args.get("latency-stats"); v.has_value()) {
      if (*v == "on" || *v == "true" || *v == "1") {
        cfg.collect_latency = true;
      } else if (*v == "off" || *v == "false" || *v == "0") {
        cfg.collect_latency = false;
      } else {
        std::println(std::cerr, "benchmark: --latency-stats must be on|off, got '{}'", *v);
        std::exit(1);
      }
    }
    return cfg;
  }

  std::vector<market_profile_t> select_presets(const cli_args_t& args) {
    static constexpr std::array<const market_profile_t*, 5> all = {
      &profile_quiet_build,
      &profile_active_match,
      &profile_cancel_heavy,
      &profile_volatile,
      &profile_sweep,
    };

    const auto pick = args.get("profile").value_or(std::string_view{"all"});
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

}  // namespace

int main(int argc, char** argv) {
  cli_args_t args{argc, argv};
  if (args.has_flag("help") || args.has_flag("h")) {
    print_help(argv[0]);
    return 0;
  }

  const auto presets = select_presets(args);
  const auto runtime = resolve_runtime(args);

  std::println(std::cout, "matching::benchmark — {} preset(s), iterations = {}, warmup = {}, latency = {}",
    presets.size(), runtime.iterations, runtime.warmup_events, runtime.collect_latency ? "on" : "off");
  std::println(std::cout, "  pinning: producer={} matcher={} stats={}",
    runtime.producer_cpu.has_value() ? std::to_string(*runtime.producer_cpu) : "off",
    runtime.matcher_cpu.has_value() ? std::to_string(*runtime.matcher_cpu) : "off",
    runtime.stats_cpu.has_value() ? std::to_string(*runtime.stats_cpu) : "off");

  for (const auto& base : presets) {
    const market_profile_t profile = apply_overrides(base, args);
    auto factory = [&](counting_emitter_t emitter) {
      return clob_factory_t{default_book_capacity, std::move(emitter)};
    };
    const auto result = run_preset(profile, runtime, factory);
    print_preset_result(std::cout, result);
    std::cout.flush();
  }

  return 0;
}
