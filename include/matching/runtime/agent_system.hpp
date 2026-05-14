#pragma once

#include <stop_token>
#include <tuple>
#include <utility>

namespace matching::runtime {

  /// Composes N @ref agent_t instances and orchestrates their lifecycle. Holds a single
  /// @c std::stop_source whose token is shared with every agent's event loop at construction
  /// time, so a single @c request_stop() call (typically from the signal handler) flips every
  /// agent at once.
  ///
  /// Two distinct shutdown modes are supported, mirroring the runtime's responsibility split:
  ///   - **Graceful**: the topmost producer pushes @ref matching::shutdown_t through the data
  ///     plane. Each agent's loop exits when its handler observes the sentinel and forwards
  ///     it downstream. No call to @ref request_stop is necessary.
  ///   - **Abort**: an external trigger (signal handler, test harness) calls
  ///     @ref request_stop. Every agent's loop observes @c stop_requested() before its next
  ///     pop and exits, possibly leaving in-flight events behind.
  ///
  /// The agent_system does not own the data-plane queues; the caller composes them and
  /// captures references inside the per-loop handlers and sources. This keeps the system
  /// pure-control-plane.
  template <typename... Agents>
  class agent_system_t {
  public:
    /// Construct from a stop_source by reference plus a pack of already-built agents.
    /// The agents must have been built using @c stop_source.get_token() so they observe the
    /// abort path; this is the convention enforced by the binary that owns this system.
    explicit agent_system_t(std::stop_source& source, Agents... agents)
      : stop_source_(&source), agents_(std::move(agents)...) {}

    agent_system_t(const agent_system_t&) = delete;
    agent_system_t& operator=(const agent_system_t&) = delete;
    agent_system_t(agent_system_t&&) noexcept = default;
    agent_system_t& operator=(agent_system_t&&) noexcept = default;

    /// Start every agent in declaration order. Threads are launched immediately and begin
    /// processing events as soon as they appear in their respective sources.
    void start() {
      start_impl(std::index_sequence_for<Agents...>{});
    }

    /// Request graceful abort on every agent. Cooperative: each loop will observe the stop
    /// before its next pop. The caller still needs to @ref join to wait for completion.
    void request_stop() noexcept {
      stop_source_->request_stop();
    }

    /// Join every agent in reverse declaration order — the convention is that the last
    /// agent declared sits at the tail of the data-plane chain, so reverse order drains
    /// downstream-first and avoids one upstream agent blocking on a dead consumer.
    void join() {
      join_impl(std::make_index_sequence<sizeof...(Agents)>{});
    }

    [[nodiscard]] std::stop_source& stop_source() noexcept {
      return *stop_source_;
    }

  private:
    template <std::size_t... Is>
    void start_impl(std::index_sequence<Is...>) {
      (std::get<Is>(agents_).start(), ...);
    }

    template <std::size_t... Is>
    void join_impl(std::index_sequence<Is...>) {
      // Reverse: last declared joins first (downstream-first drain).
      constexpr std::size_t n = sizeof...(Is);
      (std::get<n - 1 - Is>(agents_).join(), ...);
    }

    std::stop_source* stop_source_{nullptr};
    std::tuple<Agents...> agents_;
  };

  /// CTAD helper.
  template <typename... Agents>
  agent_system_t(std::stop_source&, Agents...) -> agent_system_t<Agents...>;

}  // namespace matching::runtime
