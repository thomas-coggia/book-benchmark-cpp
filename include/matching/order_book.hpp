#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "matching/input_event.hpp"
#include "matching/output_event.hpp"

namespace matching {

  /// One resting order: single cache-line-friendly record with scalar fields and intrusive
  /// same-price list pointers. Stable address for the lifetime of the @ref clob_t (resting-order
  /// @c monotonic_buffer_resource arena); pointers in levels and @c lookup_ are never invalidated by growth.
  struct alignas(64) order_node_t {
    order_id_t order_id{};
    price_t price{};
    quantity_t quantity{};
    side_t side{};
    bool active{true};
    order_node_t* prev_same_price{nullptr};
    order_node_t* next_same_price{nullptr};
  };

  namespace detail {

    /// Round @p size up to the next multiple of @p alignment (satisfies PMR @c allocate sizing).
    [[nodiscard]] constexpr std::size_t align_up(std::size_t size, std::size_t alignment) noexcept {
      return ((size + alignment - 1) / alignment) * alignment;
    }

    [[nodiscard]] constexpr std::size_t resting_order_arena_byte_count(std::size_t max_orders) noexcept {
      if (max_orders == 0) {
        return 0;
      }
      constexpr std::size_t line = 64;
      const std::size_t per = align_up(sizeof(order_node_t), line);
      return align_up(max_orders * per, 4096);
    }

    /// Single contiguous backing block whose initial space seeds @c ladder_lookup_pool: @c pmr::map
    /// nodes for bid/ask price ladders and @c pmr::unordered_map nodes for order-id lookup both draw
    /// from that pool. Overflow uses the monotonic buffer's upstream (@c new_delete_resource).
    [[nodiscard]] constexpr std::size_t ladder_lookup_arena_byte_count(std::size_t capacity) noexcept {
      constexpr std::size_t per_level = 96;
      constexpr std::size_t ceiling = 32 * 1024 * 1024;
      constexpr std::size_t floor = 262144;
      const std::size_t scaled =
        std::min<std::size_t>(std::max<std::size_t>(capacity, 16) * per_level, ceiling) + floor;
      return align_up(scaled, 4096);
    }

  }  // namespace detail

  /// Polymorphic allocator for the order-id lookup table (same @c ladder_lookup_pool as price ladders).
  using ladder_lookup_allocator_t = std::pmr::polymorphic_allocator<std::pair<const order_id_t, order_node_t*>>;

  /// Intrusive FIFO chain head/tail for orders at one price (the @c std::map key on the side).
  struct price_level_t {
    order_node_t* head{nullptr};
    order_node_t* tail{nullptr};
  };

  /// PMR allocator for one price ladder (@c bids_ and @c asks_ share this type).
  using order_book_level_allocator_t = std::pmr::polymorphic_allocator<std::pair<const price_t, price_level_t>>;

  /// Comparator for the resting-price @c std::pmr::map so the best level is always @c begin():
  /// asks sort ascending (lowest ask first); bids sort descending (highest bid first).
  /// @ref order_book_side_t::crosses uses the same comparator (negated strict ordering between limit prices).
  template <side_t Side>
  using resting_levels_compare_t = std::conditional_t<Side == side_t::buy, std::greater<price_t>, std::less<price_t>>;

  /// One side of the book (bids or asks). Levels live in a @c std::pmr::map keyed by price with
  /// @ref resting_levels_compare_t so best-first matching always uses @c begin(), using the supplied
  /// @c order_book_level_allocator_t (typically from @ref clob_memory_t::ladder_allocator).
  /// Resting orders are @ref order_node_t records linked intrusively per level.
  ///
  /// @tparam Side compile-time resting side (@c side_t::buy or @c side_t::sell); ladder ordering and
  /// @ref crosses both derive from @ref resting_levels_compare_t.
  template <side_t Side>
  class order_book_side_t {
  public:
    explicit order_book_side_t(order_book_level_allocator_t alloc) : levels_(alloc) {}

    /// True if the resting order at @p resting_price would cross an aggressive order on this
    /// side priced at @p aggressive_price. Uses @ref resting_levels_compare_t negated on the two
    /// limits (@c asks: @c !(aggressive < resting); @c bids: @c !(aggressive > resting)).
    [[nodiscard]] bool crosses(price_t aggressive_price, price_t resting_price) const noexcept {
      return !resting_levels_compare_t<Side>{}(aggressive_price, resting_price);
    }

    /// Append @p order to the tail of its price level (creating the level if needed),
    /// maintaining FIFO time priority within the level.
    void append(order_node_t* order) {
      const price_t price = order->price;
      price_level_t& level = levels_.try_emplace(price).first->second;

      if (level.tail == nullptr) {
        level.head = order;
        level.tail = order;
        order->prev_same_price = nullptr;
        order->next_same_price = nullptr;
      } else {
        order->prev_same_price = level.tail;
        order->next_same_price = nullptr;
        level.tail->next_same_price = order;
        level.tail = order;
      }
    }

    /// Detach @p order from its price level. If the level becomes empty it is dropped.
    /// The order's own active flag is left to the caller (cancel vs. fill have different
    /// retire semantics).
    void detach(order_node_t* order) noexcept {
      const price_t price = order->price;
      const auto it = levels_.find(price);
      if (it == levels_.end()) {
        return;
      }

      order_node_t* prev = order->prev_same_price;
      order_node_t* next = order->next_same_price;

      if (prev != nullptr) {
        prev->next_same_price = next;
      } else {
        it->second.head = next;
      }
      if (next != nullptr) {
        next->prev_same_price = prev;
      } else {
        it->second.tail = prev;
      }

      order->prev_same_price = nullptr;
      order->next_same_price = nullptr;

      if (it->second.head == nullptr) {
        levels_.erase(it);
      }
    }

    /// Best level: always the map minimum under @ref resting_levels_compare_t (best bid / best ask).
    [[nodiscard]] std::optional<std::pair<price_t, price_level_t*>> best_level() noexcept {
      if (levels_.empty()) {
        return std::nullopt;
      }
      auto it = levels_.begin();
      return std::pair<price_t, price_level_t*>{it->first, &it->second};
    }

    /// Drop the best level. Used after the matching loop fully consumes it; callers must
    /// already have unlinked all orders from the level's intrusive chain.
    void pop_best_level() noexcept {
      if (!levels_.empty()) {
        levels_.erase(levels_.begin());
      }
    }

    [[nodiscard]] bool empty() const noexcept {
      return levels_.empty();
    }

    [[nodiscard]] std::size_t depth() const noexcept {
      return levels_.size();
    }

  private:
    std::pmr::map<price_t, price_level_t, resting_levels_compare_t<Side>> levels_;
  };

  /// Owns PMR backing for one @ref clob_t and exposes the polymorphic allocators / resources used by
  /// the book. Typically constructed in @ref clob_factory_t (or tests) and moved into @c clob_t.
  ///
  /// Non-relocatable PMR objects live in a private heap block so @c clob_t can be moved while
  /// @c ladder_lookup_pool and @c resting_order_mono keep stable addresses.
  class clob_memory_t {
    struct impl {
      std::size_t book_capacity;
      std::vector<std::byte> ladder_lookup_arena;
      std::pmr::monotonic_buffer_resource ladder_lookup_mono;
      std::pmr::unsynchronized_pool_resource ladder_lookup_pool;
      std::vector<std::byte> resting_order_arena;
      std::pmr::monotonic_buffer_resource resting_order_mono;

      explicit impl(std::size_t capacity)
        : book_capacity(capacity),
          ladder_lookup_arena(detail::ladder_lookup_arena_byte_count(capacity)),
          ladder_lookup_mono(ladder_lookup_arena.data(), ladder_lookup_arena.size(), std::pmr::new_delete_resource()),
          ladder_lookup_pool(&ladder_lookup_mono),
          resting_order_arena(detail::resting_order_arena_byte_count(capacity)),
          resting_order_mono(resting_order_arena.data(), resting_order_arena.size(), std::pmr::new_delete_resource()) {}
    };

  public:
    explicit clob_memory_t(std::size_t book_capacity) : storage_(std::make_unique<impl>(book_capacity)) {}

    /// Capacity hint this object was built with (arena sizing); used e.g. for @c lookup_ reserve.
    [[nodiscard]] std::size_t book_capacity() const noexcept {
      return storage_->book_capacity;
    }

    [[nodiscard]] order_book_level_allocator_t ladder_allocator() const noexcept {
      return order_book_level_allocator_t{&storage_->ladder_lookup_pool};
    }

    [[nodiscard]] ladder_lookup_allocator_t lookup_allocator() const noexcept {
      return ladder_lookup_allocator_t{&storage_->ladder_lookup_pool};
    }

    /// Monotonic resource for resting @ref order_node_t allocations (separate arena from ladder/lookup pool).
    [[nodiscard]] std::pmr::memory_resource* resting_order_resource() noexcept {
      return &storage_->resting_order_mono;
    }

    clob_memory_t(const clob_memory_t&) = delete;
    clob_memory_t& operator=(const clob_memory_t&) = delete;
    clob_memory_t(clob_memory_t&&) noexcept = default;
    clob_memory_t& operator=(clob_memory_t&&) noexcept = default;

  private:
    std::unique_ptr<impl> storage_;
  };

  /// The single-symbol Central Limit Order Book.
  ///
  /// **Allocation model** (bounded by @ref clob_memory_t sizing): @ref order_node_t objects are
  /// allocated from @c resting_order_mono over @c resting_order_arena (@c detail::resting_order_arena_byte_count).
  /// Bid/ask price ladders use @c std::pmr::map with side-specific ordering (@c std::greater for bids,
  /// @c std::less for asks) so best-first traversal is always @c begin(), backed by @c ladder_lookup_pool
  /// (seeded from @c ladder_lookup_mono / @c ladder_lookup_arena; @c detail::ladder_lookup_arena_byte_count).
  /// Order-id
  /// lookup is a @c std::pmr::unordered_map sharing that same pool. Resting slots are not reused after retire.
  /// @ref clob_memory_t exposes polymorphic allocators and @c resting_order_resource; it is usually built
  /// by @ref clob_factory_t.
  ///
  /// Templated on @c Emitter so the same matching path serves both the production binary
  /// (where @c Emitter writes formatted lines to @c std::cout) and tests (where @c Emitter
  /// collects events into vectors). The emitter must provide @c operator() for
  /// @ref trade_event_t, @ref order_fully_filled_t, @ref order_partially_filled_t, and
  /// @ref order_error_event_t.
  ///
  /// **Match-loop output ordering:** for every fill we emit
  ///   1. @ref trade_event_t — the trade itself,
  ///   2. OrderFullyFilled or OrderPartiallyFilled for the **aggressive** order,
  ///   3. OrderFullyFilled or OrderPartiallyFilled for the **resting** order.
  /// Any leftover quantity on the aggressive becomes a new resting order with no fill
  /// line of its own — only orders involved in a trade emit fill events.
  template <typename Emitter>
  class clob_t {
  public:
    /// @p memory holds PMR arenas and allocators (typically from @ref clob_factory_t).
    explicit clob_t(clob_memory_t memory, Emitter emitter)
      : memory_(std::move(memory)),
        bids_{memory_.ladder_allocator()},
        asks_{memory_.ladder_allocator()},
        lookup_(memory_.lookup_allocator()),
        emitter_(std::move(emitter)) {
      lookup_.reserve(std::max<std::size_t>(memory_.book_capacity() * 2, 16));
    }

    clob_t(const clob_t&) = delete;
    clob_t& operator=(const clob_t&) = delete;
    clob_t(clob_t&&) noexcept = default;
    clob_t& operator=(clob_t&&) noexcept = default;

    /// Process a parsed input event. The @c std::visit cost compiles down to a single
    /// branch on the variant index for our alternatives.
    void operator()(const input_event_t& event) {
      std::visit([this](const auto& concrete) { (*this)(concrete); }, event);
    }

    /// Add order event path: try to match the incoming order against the opposite side, then
    /// rest any residual quantity on its own side.
    ///
    /// Stateless field checks (positive @c order_id, @c quantity, @c price) emit
    /// @ref order_error_event_t and skip matching; internal book invariants remain @c assert-gated.
    void operator()(const add_order_event_t& event) {
      if (event.order_id <= 0) {
        emitter_(order_error_event_t{event.order_id, order_error_kind_t::invalid_add_order_id});
        return;
      }
      if (event.quantity <= 0) {
        emitter_(order_error_event_t{event.order_id, order_error_kind_t::invalid_add_quantity});
        return;
      }
      if (event.price <= 0) {
        emitter_(order_error_event_t{event.order_id, order_error_kind_t::invalid_add_price});
        return;
      }

      add_order_event_t residual = event;
      if (event.side == side_t::buy) {
        match_against(asks_, residual);
      } else {
        match_against(bids_, residual);
      }

      if (residual.quantity > 0) {
        rest_order(residual);
      }
    }

    /// Cancel order event path: @c std::pmr::unordered_map for ids; @c pmr::map for prices.
    void operator()(const cancel_order_event_t& event) {
      if (event.order_id <= 0) {
        emitter_(order_error_event_t{event.order_id, order_error_kind_t::invalid_cancel_order_id});
        return;
      }
      const auto it = lookup_.find(event.order_id);
      if (it == lookup_.end() || !it->second->active) {
        emitter_(order_error_event_t{event.order_id, order_error_kind_t::unknown_order_id});
        return;
      }
      order_node_t* node = it->second;
      if (node->side == side_t::buy) {
        bids_.detach(node);
      } else {
        asks_.detach(node);
      }
      node->active = false;
      lookup_.erase(it);
    }

    /// Shutdown sentinel: no book mutation. The matcher agent is responsible for forwarding the
    /// sentinel to the next stage before exiting; this overload exists only so the variant
    /// dispatch in @c operator()(const input_event_t&) compiles uniformly across all
    /// alternatives.
    void operator()(const shutdown_t&) noexcept {}

    [[nodiscard]] const Emitter& emitter() const noexcept {
      return emitter_;
    }

    [[nodiscard]] Emitter& emitter() noexcept {
      return emitter_;
    }

    [[nodiscard]] std::size_t bid_depth() const noexcept {
      return bids_.depth();
    }

    [[nodiscard]] std::size_t ask_depth() const noexcept {
      return asks_.depth();
    }

  private:
    /// Allocates and value-initializes a node from @c memory_.resting_order_resource(). Throws on OOM.
    [[nodiscard]] order_node_t* allocate_resting_node(order_id_t id, side_t side, price_t price, quantity_t qty) {
      void* const raw = memory_.resting_order_resource()->allocate(sizeof(order_node_t), alignof(order_node_t));
      order_node_t* const node = static_cast<order_node_t*>(raw);
      std::construct_at(node);
      node->order_id = id;
      node->price = price;
      node->quantity = qty;
      node->side = side;
      node->active = true;
      node->prev_same_price = nullptr;
      node->next_same_price = nullptr;
      return node;
    }

    /// Walk price levels best-first, draining each level's intrusive chain until either the
    /// aggressive @p incoming is filled or the next level no longer crosses.
    template <side_t OppositeSide>
    void match_against(order_book_side_t<OppositeSide>& opposite, add_order_event_t& incoming) {
      while (incoming.quantity > 0) {
        const auto best = opposite.best_level();
        if (!best) {
          return;
        }
        const auto [level_price, level] = *best;
        if (!opposite.crosses(incoming.price, level_price)) {
          return;
        }
        match_level(opposite, *level, level_price, incoming);
        if (level->head == nullptr) {
          opposite.pop_best_level();
        }
      }
    }

    /// Drain @p level head-first, trading @c min(aggressive, resting) per step. Removes
    /// fully-filled resting orders from the chain in place, but leaves the empty-level
    /// erasure to the caller so we never invalidate the iterator we were about to bump.
    template <side_t OppositeSide>
    void match_level(
      order_book_side_t<OppositeSide>& opposite,
      price_level_t& level,
      price_t level_price,
      add_order_event_t& incoming
    ) {
      order_node_t* cursor = level.head;
      while (cursor != nullptr && incoming.quantity > 0) {
        order_node_t* const resting = cursor;
        order_node_t* const next = resting->next_same_price;

        assert(resting->active);

        const quantity_t resting_qty = resting->quantity;
        const quantity_t trade_qty = std::min(incoming.quantity, resting_qty);
        const price_t trade_price = level_price;
        const order_id_t resting_id = resting->order_id;

        const quantity_t aggressive_remaining = incoming.quantity - trade_qty;
        const quantity_t resting_remaining = resting_qty - trade_qty;

        emitter_(trade_event_t{trade_qty, trade_price});
        emit_fill(incoming.order_id, aggressive_remaining);
        emit_fill(resting_id, resting_remaining);

        incoming.quantity = aggressive_remaining;

        if (resting_remaining == 0) {
          retire_resting(opposite, level, resting);
        } else {
          resting->quantity = resting_remaining;
        }

        cursor = next;
      }
    }

    void emit_fill(order_id_t id, quantity_t remaining) {
      if (remaining == 0) {
        emitter_(order_fully_filled_t{id});
      } else {
        emitter_(order_partially_filled_t{id, remaining});
      }
    }

    template <side_t OppositeSide>
    void retire_resting(order_book_side_t<OppositeSide>& opposite, price_level_t& level, order_node_t* node) noexcept {
      order_node_t* const prev = node->prev_same_price;
      order_node_t* const next = node->next_same_price;
      if (prev != nullptr) {
        prev->next_same_price = next;
      } else {
        level.head = next;
      }
      if (next != nullptr) {
        next->prev_same_price = prev;
      } else {
        level.tail = prev;
      }
      node->prev_same_price = nullptr;
      node->next_same_price = nullptr;
      node->active = false;
      lookup_.erase(node->order_id);
      // Caller (@c match_against) drops the level itself when it sees @c level.head == nullptr.
      (void)opposite;
    }

    void rest_order(const add_order_event_t& residual) {
      order_node_t* const node =
        allocate_resting_node(residual.order_id, residual.side, residual.price, residual.quantity);
      if (!lookup_.try_emplace(residual.order_id, node).second) {
        // Duplicate id. Roll back the node we just claimed so a conflicting event does not corrupt the book.
        emitter_(order_error_event_t{residual.order_id, order_error_kind_t::duplicate_order_id});
        node->active = false;
        return;
      }
      if (residual.side == side_t::buy) {
        bids_.append(node);
      } else {
        asks_.append(node);
      }
    }

    clob_memory_t memory_;
    order_book_side_t<side_t::buy> bids_;
    order_book_side_t<side_t::sell> asks_;
    std::pmr::unordered_map<order_id_t, order_node_t*> lookup_;
    Emitter emitter_;
  };

}  // namespace matching
