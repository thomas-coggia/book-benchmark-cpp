#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <print>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "matching/benchmark/benchmark_timer.hpp"
#include "matching/benchmark/market_profile.hpp"
#include "matching/benchmark/order_generator.hpp"
#include "matching/benchmark/statistics.hpp"
#include "matching/input_event.hpp"
#include "matching/output_event.hpp"
#include "matching/runtime/agent.hpp"
#include "matching/runtime/agent_system.hpp"
#include "matching/runtime/event_loop.hpp"
#include "matching/runtime/spsc_queue.hpp"

namespace matching::benchmark {

  /// Harness-only settings for a benchmark run: warmup, iteration count, optional CPU pinning, and
  /// latency collection. Populated from the benchmark executable CLI alongside resolved profiles.
  struct benchmark_config_t {
    std::size_t warmup_events{10'000};
    std::size_t iterations{5};
    bool collect_latency{true};
    std::optional<int> producer_cpu{};
    std::optional<int> matcher_cpu{};
    std::optional<int> stats_cpu{};
  };

  /// Outputs the matcher tracks during the timed window. Trade count is incremented on the
  /// matcher thread (no synchronisation), then read on the main thread after the agent
  /// system joins — the join boundary publishes the counter cleanly.
  struct counting_emitter_t {
    std::size_t* trade_count{nullptr};

    void operator()(const trade_event_t&) const noexcept {
      ++*trade_count;
    }
    void operator()(const order_fully_filled_t&) const noexcept {}
    void operator()(const order_partially_filled_t&) const noexcept {}
  };

  /// One latency sample shipped from the matcher to the stats agent: elapsed time for one
  /// `book(event)` call in nanoseconds (via @ref bench_elapsed_ns).
  struct ping_t {
    double latency_ns{0.0};
  };

  /// Stats-queue payload: either a real latency sample or the same @ref matching::shutdown_t
  /// sentinel used on the order queue (end of pipeline on this channel).
  using stats_event_t = std::variant<ping_t, shutdown_t>;

  /// Per-iteration result. Aggregated across iterations into @ref preset_result_t.
  struct iteration_result_t {
    double elapsed_ns{0.0};
    std::size_t orders_processed{0};
    std::size_t trades_generated{0};
    latency_stats_t latency{};
  };

  /// Final, printable result for one preset across all iterations.
  struct preset_result_t {
    market_profile_t profile{};
    std::size_t total_orders{0};
    double mean_orders_per_sec{0.0};
    double mean_trades_per_sec{0.0};
    double total_elapsed_ns{0.0};
    std::size_t total_trades{0};
    latency_stats_t latency{};
  };

  namespace detail {

    /// Default queue depth for the benchmark's SPSC pair. 65,536 leaves enough head-room
    /// for the matcher to lag the producer by tens of milliseconds without stalling, while
    /// keeping queue memory under one MiB even with the largest event variant.
    inline constexpr std::size_t queue_capacity_v = 1u << 16;

    using order_queue_t = runtime::spsc_queue_t<input_event_t, queue_capacity_v>;
    using stats_queue_t = runtime::spsc_queue_t<stats_event_t, queue_capacity_v>;

    /// Producer agent: pushes pre-generated events into the order queue in a tight loop,
    /// then injects a single @ref matching::shutdown_t to terminate the downstream chain.
    /// The push spin checks the stop token so an abort does not deadlock when the matcher
    /// has already stopped consuming.
    class producer_loop_t {
    public:
      producer_loop_t(
        const std::vector<input_event_t>& events,
        std::size_t begin,
        std::size_t end,
        std::shared_ptr<order_queue_t> queue,
        std::stop_token token
      ) noexcept
        : events_(&events),
          begin_(begin),
          end_(end),
          queue_(std::move(queue)),
          token_(std::move(token)) {}

      void run() {
        for (std::size_t i = begin_; i < end_; ++i) {
          push((*events_)[i]);
          if (token_.stop_requested()) {
            return;
          }
        }
        push(input_event_t{shutdown_t{}});
      }

    private:
      void push(const input_event_t& event) noexcept {
        while (!token_.stop_requested() && !queue_->try_push(event)) {
          runtime::cpu_pause();
        }
      }

      const std::vector<input_event_t>* events_{nullptr};
      std::size_t begin_{0};
      std::size_t end_{0};
      std::shared_ptr<order_queue_t> queue_;
      std::stop_token token_;
    };

    /// Matcher handler: brackets every real event with @ref bench_mark / @ref bench_elapsed_ns,
    /// ships nanoseconds as @ref ping_t to the stats queue, then forwards @ref matching::shutdown_t
    /// before terminating.
    /// The latency sample is only collected when @c collect_latency is true; otherwise the
    /// matcher remains a single-pop / single-call tight loop.
    template <typename Book>
    class matcher_handler_t {
    public:
      matcher_handler_t(
        Book book,
        std::shared_ptr<stats_queue_t> stats,
        bool collect_latency,
        std::stop_token token
      ) noexcept(std::is_nothrow_move_constructible_v<Book>)
        : book_(std::move(book)),
          stats_(std::move(stats)),
          collect_latency_(collect_latency),
          token_(std::move(token)) {}

      [[nodiscard]] bool operator()(const input_event_t& event) noexcept {
        if (std::holds_alternative<shutdown_t>(event)) {
          forward_shutdown();
          return true;
        }
        if (collect_latency_) {
          const std::uint64_t mark = bench_mark();
          book_(event);
          push(stats_event_t{ping_t{bench_elapsed_ns(mark)}});
        } else {
          book_(event);
        }
        return false;
      }

    private:
      void forward_shutdown() noexcept {
        push(stats_event_t{shutdown_t{}});
      }

      void push(const stats_event_t& event) noexcept {
        while (!token_.stop_requested() && !stats_->try_push(event)) {
          runtime::cpu_pause();
        }
      }

      Book book_;
      std::shared_ptr<stats_queue_t> stats_;
      bool collect_latency_{true};
      std::stop_token token_;
    };

    /// Stats handler: pops ping_t / shutdown_t and feeds the accumulator.
    class stats_handler_t {
    public:
      explicit stats_handler_t(latency_accumulator_t& acc) noexcept : acc_(&acc) {}

      [[nodiscard]] bool operator()(const stats_event_t& event) noexcept {
        if (std::holds_alternative<shutdown_t>(event)) {
          return true;
        }
        const auto& ping = std::get<ping_t>(event);
        acc_->add(ping.latency_ns);
        return false;
      }

    private:
      latency_accumulator_t* acc_{nullptr};
    };

  }  // namespace detail

  /// Runs @ref clob_t through a single preset for @c iterations rounds and aggregates
  /// throughput / latency. @p make_factory must return a fresh book for each timed iteration
  /// (typically @ref clob_factory_t::create).
  ///
  /// **Topology**:
  /// @code
  ///   [producer agent] --input_event_t--> [matcher agent] --ping_t--> [stats agent]
  ///                     spsc_queue<...>                    spsc_queue<...>
  /// @endcode
  /// The matcher timestamps each event with @ref bench_mark / @ref bench_elapsed_ns and pushes
  /// @ref ping_t; the latency accumulator runs on the stats agent so Welford / reservoir cost
  /// cannot inflate the reported per-event latency.
  template <typename MakeFactory>
  [[nodiscard]] inline preset_result_t run_preset(
    const market_profile_t& profile,
    const benchmark_config_t& cfg,
    MakeFactory&& make_factory
  ) {
    benchmark_timer_init();

    preset_result_t agg{};
    agg.profile = profile;

    latency_accumulator_t latency_global{profile.seed};

    for (std::size_t iter = 0; iter < cfg.iterations; ++iter) {
      // Independent generator per iteration: same seed family so the run is reproducible
      // from the printed parameters, but each iteration starts from a clean book.
      market_profile_t per_iter = profile;
      per_iter.seed = profile.seed + static_cast<std::uint64_t>(iter);
      order_generator_t gen{per_iter};

      const std::size_t total_events = cfg.warmup_events + per_iter.num_orders;
      std::vector<input_event_t> events;
      events.reserve(total_events);
      for (std::size_t i = 0; i < total_events; ++i) {
        events.emplace_back(gen.next());
      }

      std::size_t trade_counter = 0;
      counting_emitter_t emitter{&trade_counter};
      auto book = std::move(make_factory(emitter)).create();

      // Warmup phase: drive the book directly with no agent overhead, no timing.
      for (std::size_t i = 0; i < cfg.warmup_events; ++i) {
        book(events[i]);
      }
      trade_counter = 0;

      const auto order_queue = std::make_shared<detail::order_queue_t>();
      const auto stats_queue = std::make_shared<detail::stats_queue_t>();

      std::stop_source source;
      const std::stop_token token = source.get_token();

      using book_type = std::remove_reference_t<decltype(book)>;
      detail::producer_loop_t producer{events, cfg.warmup_events, total_events, order_queue, token};
      auto matcher_loop = runtime::make_event_loop(
        runtime::queue_source_shared_t<detail::order_queue_t>{order_queue},
        detail::matcher_handler_t<book_type>{std::move(book), stats_queue, cfg.collect_latency, token},
        token
      );
      auto stats_loop = runtime::make_event_loop(
        runtime::queue_source_shared_t<detail::stats_queue_t>{stats_queue},
        detail::stats_handler_t{latency_global},
        token
      );

      auto producer_agent = runtime::make_agent(std::move(producer), cfg.producer_cpu);
      auto matcher_agent = runtime::make_agent(std::move(matcher_loop), cfg.matcher_cpu);
      auto stats_agent = runtime::make_agent(std::move(stats_loop), cfg.stats_cpu);

      auto system = runtime::make_agent_system(
        source,
        std::move(producer_agent),
        std::move(matcher_agent),
        std::move(stats_agent)
      );

      const std::uint64_t window_start = bench_mark();
      system.start();
      system.join();
      const double window_ns = bench_elapsed_ns(window_start);

      agg.total_orders += per_iter.num_orders;
      agg.total_trades += trade_counter;
      agg.total_elapsed_ns += window_ns;
    }

    if (agg.total_elapsed_ns > 0.0) {
      const double secs = agg.total_elapsed_ns / 1e9;
      agg.mean_orders_per_sec = static_cast<double>(agg.total_orders) / secs;
      agg.mean_trades_per_sec = static_cast<double>(agg.total_trades) / secs;
    }
    agg.latency = latency_global.snapshot();
    return agg;
  }

  /// Print one preset's resolved parameters and aggregated metrics. Each block is
  /// self-contained so a reviewer can reproduce any printed number from the header and field
  /// list alone.
  inline void print_preset_result(std::ostream& out, const preset_result_t& r) {
    const auto& p = r.profile;
    std::println(out, "============================================================");
    std::println(out, "Preset: {}", p.name);
    std::println(out, "------------------------------------------------------------");
    std::println(out, "Resolved profile:");
    std::println(out, "  seed              = {}", p.seed);
    std::println(out, "  num_orders        = {}", p.num_orders);
    std::println(out, "  cancel_ratio      = {:.4f}", p.cancel_ratio);
    std::println(out, "  aggressive_ratio  = {:.4f}", p.aggressive_ratio);
    std::println(out, "  buy_bias          = {:.4f}", p.buy_bias);
    std::println(out, "  mu                = {:.6f}", p.mu);
    std::println(out, "  sigma             = {:.6f}", p.sigma);
    std::println(out, "  initial_mid       = {}", p.initial_mid);
    std::println(out, "  tick_size         = {}", p.tick_size);
    std::println(out, "  place_decay       = {:.4f}", p.place_decay);
    std::println(out, "  qty_log_mean      = {:.4f}", p.qty_log_mean);
    std::println(out, "  qty_log_stddev    = {:.4f}", p.qty_log_stddev);
    std::println(out, "  qty_min/qty_max   = {} / {}", p.qty_min, p.qty_max);
    std::println(out, "------------------------------------------------------------");
    std::println(out, "Throughput:");
    std::println(out, "  orders            = {}", r.total_orders);
    std::println(out, "  trades            = {}", r.total_trades);
    std::println(out, "  total elapsed     = {:.3f} ms", r.total_elapsed_ns / 1e6);
    std::println(out, "  orders / sec      = {:.2f}", r.mean_orders_per_sec);
    std::println(out, "  trades / sec      = {:.2f}", r.mean_trades_per_sec);
    std::println(out, "Latency (ns):");
    std::println(out, "  samples           = {}", r.latency.sample_count);
    std::println(out, "  mean              = {:.1f}", r.latency.mean_ns);
    std::println(out, "  stddev            = {:.1f}", r.latency.stddev_ns);
    std::println(out, "  p10 / p50         = {:.1f} / {:.1f}", r.latency.p10_ns, r.latency.p50_ns);
    std::println(out, "  p95 / p99 / p99.9 = {:.1f} / {:.1f} / {:.1f}", r.latency.p95_ns, r.latency.p99_ns, r.latency.p99_9_ns);
    std::println(out, "  min / max         = {:.1f} / {:.1f}", r.latency.min_ns, r.latency.max_ns);
    std::println(out, "");
  }

}  // namespace matching::benchmark
