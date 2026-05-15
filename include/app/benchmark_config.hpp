#pragma once

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

namespace benchmark_config {

  /// Fully resolved benchmark run: harness settings plus market profiles (CLI overrides merged).
  struct config_t {
    matching::benchmark::benchmark_config_t benchmark{};
    std::vector<matching::benchmark::market_profile_t> profiles{};
  };

  /// `--help` / `-h`: usage was printed to stdout; exit with status 0.
  struct help_requested {};

  /// Parse or validation failed; caller should exit non-zero (message already on stderr).
  struct parse_error {};

  namespace detail {

    struct market_profile_overrides_t {
      std::optional<std::size_t> orders{};
      std::optional<std::uint64_t> seed{};
      std::optional<double> cancel_ratio{};
      std::optional<double> aggressive_ratio{};
      std::optional<double> buy_bias{};
      std::optional<double> mu{};
      std::optional<double> sigma{};
      std::optional<std::int32_t> initial_mid{};
      std::optional<std::int32_t> tick_size{};
      std::optional<double> place_decay{};
      std::optional<double> qty_log_mean{};
      std::optional<double> qty_log_stddev{};
      std::optional<std::int32_t> qty_min{};
      std::optional<std::int32_t> qty_max{};
    };

    struct staging_t {
      std::string profile_selection{"all"};
      matching::benchmark::benchmark_config_t benchmark{};
      market_profile_overrides_t profile_overrides{};
    };

    inline constexpr std::array<std::pair<std::string_view, std::string_view>, 20> flag_docs = {{
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

    [[nodiscard]] inline cxxopts::Options build_options(const char* program_name) {
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

    template <typename T>
    void assign_if_present(const cxxopts::ParseResult& result, const char* flag, T& slot) {
      if (result.count(flag) == 0) {
        return;
      }
      slot = result[flag].as<T>();
    }

    template <typename T>
    void assign_optional_field(const cxxopts::ParseResult& result, const char* flag, std::optional<T>& slot) {
      if (result.count(flag) == 0) {
        return;
      }
      slot = result[flag].as<T>();
    }

    inline void
    assign_cpu(const cxxopts::ParseResult& result, const char* flag, std::optional<int>& slot, bool& failed) {
      if (result.count(flag) == 0) {
        return;
      }
      try {
        const int v = result[flag].as<int>();
        if (v < 0) {
          std::println(std::cerr, "benchmark: invalid value for --{}: '{}' (expected non-negative integer)", flag, v);
          failed = true;
          return;
        }
        slot = v;
      } catch (const cxxopts::exceptions::exception& ex) {
        std::println(std::cerr, "benchmark: invalid value for --{}: {}", flag, ex.what());
        failed = true;
      }
    }

    inline void fill_staging_from_parse(const cxxopts::ParseResult& parsed, staging_t& out) {
      bool failed = false;
      try {
        if (parsed.count("profile") != 0) {
          out.profile_selection = parsed["profile"].as<std::string>();
        }

        assign_if_present(parsed, "warmup", out.benchmark.warmup_events);
        assign_if_present(parsed, "iterations", out.benchmark.iterations);

        assign_cpu(parsed, "producer-cpu", out.benchmark.producer_cpu, failed);
        if (failed) {
          throw parse_error{};
        }
        assign_cpu(parsed, "matcher-cpu", out.benchmark.matcher_cpu, failed);
        if (failed) {
          throw parse_error{};
        }
        assign_cpu(parsed, "stats-cpu", out.benchmark.stats_cpu, failed);
        if (failed) {
          throw parse_error{};
        }

        if (parsed.count("latency-stats") != 0) {
          const std::string v = parsed["latency-stats"].as<std::string>();
          if (v == "on" || v == "true" || v == "1") {
            out.benchmark.collect_latency = true;
          } else if (v == "off" || v == "false" || v == "0") {
            out.benchmark.collect_latency = false;
          } else {
            std::println(std::cerr, "benchmark: --latency-stats must be on|off, got '{}'", v);
            throw parse_error{};
          }
        }

        auto& o = out.profile_overrides;
        assign_optional_field(parsed, "orders", o.orders);
        assign_optional_field(parsed, "seed", o.seed);
        assign_optional_field(parsed, "cancel-ratio", o.cancel_ratio);
        assign_optional_field(parsed, "aggressive-ratio", o.aggressive_ratio);
        assign_optional_field(parsed, "buy-bias", o.buy_bias);
        assign_optional_field(parsed, "mu", o.mu);
        assign_optional_field(parsed, "sigma", o.sigma);
        assign_optional_field(parsed, "initial-mid", o.initial_mid);
        assign_optional_field(parsed, "tick-size", o.tick_size);
        assign_optional_field(parsed, "place-decay", o.place_decay);
        assign_optional_field(parsed, "qty-log-mean", o.qty_log_mean);
        assign_optional_field(parsed, "qty-log-stddev", o.qty_log_stddev);
        assign_optional_field(parsed, "qty-min", o.qty_min);
        assign_optional_field(parsed, "qty-max", o.qty_max);
      } catch (const cxxopts::exceptions::exception& ex) {
        std::println(std::cerr, "benchmark: {}", ex.what());
        throw parse_error{};
      }
    }

    [[nodiscard]] inline matching::benchmark::market_profile_t
    merge_staged_overrides(matching::benchmark::market_profile_t base, const market_profile_overrides_t& o) {
      if (o.orders) {
        base.num_orders = *o.orders;
      }
      if (o.seed) {
        base.seed = *o.seed;
      }
      if (o.cancel_ratio) {
        base.cancel_ratio = *o.cancel_ratio;
      }
      if (o.aggressive_ratio) {
        base.aggressive_ratio = *o.aggressive_ratio;
      }
      if (o.buy_bias) {
        base.buy_bias = *o.buy_bias;
      }
      if (o.mu) {
        base.mu = *o.mu;
      }
      if (o.sigma) {
        base.sigma = *o.sigma;
      }
      if (o.initial_mid) {
        base.initial_mid = *o.initial_mid;
      }
      if (o.tick_size) {
        base.tick_size = *o.tick_size;
      }
      if (o.place_decay) {
        base.place_decay = *o.place_decay;
      }
      if (o.qty_log_mean) {
        base.qty_log_mean = *o.qty_log_mean;
      }
      if (o.qty_log_stddev) {
        base.qty_log_stddev = *o.qty_log_stddev;
      }
      if (o.qty_min) {
        base.qty_min = *o.qty_min;
      }
      if (o.qty_max) {
        base.qty_max = *o.qty_max;
      }
      return base;
    }

    inline void resolve_presets(const std::string& pick, std::vector<matching::benchmark::market_profile_t>& out) {
      using matching::benchmark::market_profile_t;
      using matching::benchmark::profile_active_match;
      using matching::benchmark::profile_cancel_heavy;
      using matching::benchmark::profile_quiet_build;
      using matching::benchmark::profile_sweep;
      using matching::benchmark::profile_volatile;

      static constexpr std::array<const market_profile_t*, 5> all = {
        &profile_quiet_build,
        &profile_active_match,
        &profile_cancel_heavy,
        &profile_volatile,
        &profile_sweep,
      };

      if (pick == "all") {
        for (const auto* p : all) {
          out.push_back(*p);
        }
        return;
      }
      for (const auto* p : all) {
        if (p->name == pick) {
          out.push_back(*p);
          return;
        }
      }
      std::println(
        std::cerr, "benchmark: unknown --profile '{}'. Valid: quiet | active | cancel | volatile | sweep | all", pick
      );
      throw parse_error{};
    }

    inline void write_help(const char* program_name, std::ostream& out) {
      std::println(out, "Usage: {} [flags]", program_name);
      std::println(out, "");
      std::println(out, "Runs the matching engine through one or all five synthetic-market presets.");
      std::println(out, "");
      std::println(out, "Flags:");
      for (const auto& [flag, help] : flag_docs) {
        std::println(out, "  {:<28} {}", flag, help);
      }
      std::println(out, "  --help, -h                   print this message and exit");
    }

  }  // namespace detail

  /// Builds @ref config_t from @p argc / @p argv (cxxopts is an implementation detail).
  /// @throws help_requested after printing usage to stdout.
  /// @throws parse_error after printing a diagnostic to stderr.
  [[nodiscard]] inline config_t parse_config(int argc, char** argv) {
    const char* prog = argv != nullptr && argv[0] != nullptr ? argv[0] : "benchmark";
    cxxopts::Options options = detail::build_options(prog);

    cxxopts::ParseResult parsed;
    try {
      parsed = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception& ex) {
      std::println(std::cerr, "benchmark: {}", ex.what());
      throw parse_error{};
    }

    if (parsed.count("help") != 0) {
      detail::write_help(prog, std::cout);
      throw help_requested{};
    }

    detail::staging_t staging{};
    detail::fill_staging_from_parse(parsed, staging);

    std::vector<matching::benchmark::market_profile_t> bases{};
    detail::resolve_presets(staging.profile_selection, bases);

    std::vector<matching::benchmark::market_profile_t> profiles{};
    profiles.reserve(bases.size());
    for (const auto& base : bases) {
      profiles.push_back(detail::merge_staged_overrides(base, staging.profile_overrides));
    }

    return config_t{.benchmark = std::move(staging.benchmark), .profiles = std::move(profiles)};
  }

}  // namespace benchmark_config
