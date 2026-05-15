#pragma once

#include <cxxopts.hpp>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <print>

namespace matching_engine_config {

  /// Default max resting-order allocations (~2M); bundled sample and benchmark presets fit comfortably.
  inline constexpr std::size_t default_capacity_v = 2'000'000;

  /// Native configuration for the `matching_engine` binary (after argv is interpreted).
  struct config_t {
    std::size_t capacity = default_capacity_v;
    std::optional<int> reader_cpu{};
    std::optional<int> matcher_cpu{};
    std::optional<int> writer_cpu{};
  };

  /// `--help` / `-h`: usage was printed to stdout; exit with status 0.
  struct help_requested {};

  /// Parse or validation failed; caller should exit non-zero (message already on stderr).
  struct parse_error {};

  namespace detail {

    inline void write_help(std::ostream& out) {
      std::println(
        out,
        "matching_engine — single-symbol limit-order matching engine\n"
        "\n"
        "Reads CSV-encoded request lines from stdin, drives the order book through an\n"
        "event-driven runtime (reader → matcher → writer agents joined by SPSC queues),\n"
        "writes trade and fill events to stdout, and reports malformed input lines on stderr.\n"
        "\n"
        "Input grammar (one message per line):\n"
        "  0,<orderid>,<side>,<quantity>,<price>     AddOrderRequest    (side: 0=Buy, 1=Sell)\n"
        "  1,<orderid>                               CancelOrderRequest\n"
        "\n"
        "Output grammar:\n"
        "  2,<quantity>,<price>                       TradeEvent\n"
        "  3,<orderid>                                OrderFullyFilled\n"
        "  4,<orderid>,<remaining-quantity>           OrderPartiallyFilled\n"
        "\n"
        "Flags:\n"
        "  --capacity <N>     Maximum number of resting orders the book can allocate (default: {}).\n"
        "  --reader-cpu <N>   Pin the stdin reader agent to CPU N.\n"
        "  --matcher-cpu <N>  Pin the matcher agent to CPU N.\n"
        "  --writer-cpu <N>   Pin the output writer agent to CPU N.\n"
        "  --help, -h         Print this message and exit.\n",
        default_capacity_v
      );
    }

  }  // namespace detail

  /// Builds @ref config_t from @p argc / @p argv (cxxopts is an implementation detail).
  /// @throws help_requested after printing usage to stdout.
  /// @throws parse_error after printing a diagnostic to stderr.
  [[nodiscard]] inline config_t parse_config(int argc, char** argv) {
    cxxopts::Options options(argv != nullptr && argv[0] != nullptr ? argv[0] : "matching_engine", "");
    options.add_options()
      ("h,help", "Print detailed help and exit")
      ("capacity", "Maximum number of resting orders the book can allocate", cxxopts::value<std::size_t>())
      ("reader-cpu", "Pin the stdin reader agent to CPU N", cxxopts::value<int>())
      ("matcher-cpu", "Pin the matcher agent to CPU N", cxxopts::value<int>())
      ("writer-cpu", "Pin the output writer agent to CPU N", cxxopts::value<int>());

    cxxopts::ParseResult parsed;
    try {
      parsed = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception& ex) {
      std::println(std::cerr, "matching_engine: {}", ex.what());
      throw parse_error{};
    }

    if (parsed.count("help") != 0) {
      detail::write_help(std::cout);
      throw help_requested{};
    }

    config_t cfg{};

    if (parsed.count("capacity") != 0) {
      cfg.capacity = parsed["capacity"].as<std::size_t>();
      if (cfg.capacity == 0) {
        std::println(std::cerr, "matching_engine: invalid --capacity value '0'");
        throw parse_error{};
      }
    }

    for (const auto& [flag, holder] : std::initializer_list<std::pair<const char*, std::optional<int>*>>{
           {"reader-cpu", &cfg.reader_cpu},
           {"matcher-cpu", &cfg.matcher_cpu},
           {"writer-cpu", &cfg.writer_cpu},
         }) {
      if (parsed.count(flag) != 0) {
        const int value = parsed[flag].as<int>();
        if (value < 0) {
          std::println(std::cerr, "matching_engine: invalid --{} value '{}'", flag, value);
          throw parse_error{};
        }
        *holder = value;
      }
    }

    return cfg;
  }

}  // namespace matching_engine_config
