#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
  /// same-price list pointers. Stable address for the lifetime of the @ref clob_t (monotonic
  /// arena); pointers in levels and @c lookup_ are never invalidated by growth.
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

    /// Contiguous backing for resting @ref order_node_t objects via @c monotonic_buffer_resource.
    [[nodiscard]] constexpr std::size_t order_arena_byte_count(std::size_t max_orders) noexcept {
      if (max_orders == 0) {
        return 0;
      }
      constexpr std::size_t line = 64;
      const std::size_t per = align_up(sizeof(order_node_t), line);
      return align_up(max_orders * per, 4096);
    }

    /// Single contiguous backing block for the two @c pmr::map price ladders in @ref clob_t.
    /// Map nodes are taken from an @c unsynchronized_pool_resource seeded from this buffer; if the
    /// book grows past the estimate, the monotonic buffer's upstream (@c new_delete_resource)
    /// satisfies overflow. Capped so huge @p capacity hints do not reserve hundreds of MiB.
    [[nodiscard]] constexpr std::size_t clob_arena_byte_count(std::size_t capacity) noexcept {
      constexpr std::size_t per_level = 96;
      constexpr std::size_t ceiling = 32 * 1024 * 1024;
      constexpr std::size_t floor = 262144;
      const std::size_t scaled =
        std::min<std::size_t>(std::max<std::size_t>(capacity, 16) * per_level, ceiling) + floor;
      return align_up(scaled, 4096);
    }

  }  // namespace detail

  /// Intrusive FIFO chain head/tail for orders at one price (the @c std::map key on the side).
  struct price_level_t {
    order_node_t* head{nullptr};
    order_node_t* tail{nullptr};
  };

  /// One side of the book (bids or asks). Levels live in a @c std::pmr::map keyed by price (asks:
  /// best is @c begin, bids: best is @c rbegin), allocating from the shared pool supplied at
  /// construction. Resting orders are @ref order_node_t records linked intrusively per level.
  class order_book_side_t {
  public:
    explicit order_book_side_t(side_t side, std::pmr::memory_resource* pool)
      : side_(side),
        levels_(pool) {}

    /// True if the resting order at @p resting_price would cross an aggressive order on this
    /// side priced at @p aggressive_price.  For a buy aggressive on the ask side, we cross when
    /// the ask price is at-or-below the buy limit; mirrored for a sell aggressive on the bid side.
    [[nodiscard]] bool crosses(price_t aggressive_price, price_t resting_price) const noexcept {
      // @c side_ describes the resting side; the aggressive is the opposite.
      return (side_ == side_t::sell) ? (resting_price <= aggressive_price) : (resting_price >= aggressive_price);
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

    /// Best level: ask = lowest price, bid = highest.
    [[nodiscard]] std::optional<std::pair<price_t, price_level_t*>> best_level() noexcept {
      if (levels_.empty()) {
        return std::nullopt;
      }
      if (side_ == side_t::sell) {
        auto it = levels_.begin();
        return std::pair<price_t, price_level_t*>{it->first, &it->second};
      }
      auto it = std::prev(levels_.end());
      return std::pair<price_t, price_level_t*>{it->first, &it->second};
    }

    /// Drop the best level. Used after the matching loop fully consumes it; callers must
    /// already have unlinked all orders from the level's intrusive chain.
    void pop_best_level() noexcept {
      if (levels_.empty()) {
        return;
      }
      if (side_ == side_t::sell) {
        levels_.erase(levels_.begin());
      } else {
        levels_.erase(std::prev(levels_.end()));
      }
    }

    [[nodiscard]] bool empty() const noexcept {
      return levels_.empty();
    }

    [[nodiscard]] std::size_t depth() const noexcept {
      return levels_.size();
    }

  private:
    side_t side_{};
    std::pmr::map<price_t, price_level_t> levels_;
  };

  /// The single-symbol Central Limit Order Book.
  ///
  /// **Allocation model** (bounded by the constructor @p capacity): each resting order is an
  /// @ref order_node_t allocated from a dedicated @c std::pmr::monotonic_buffer_resource on
  /// @c order_arena_byte_count(capacity) bytes (overflow to @c new_delete_resource). Pointers to
  /// nodes stay valid for the life of the book; resting slots are not reused after retire.
  /// Bid/ask price ladders use @c std::pmr::map backed by one
  /// contiguous @c std::vector<std::byte> arena feeding an @c unsynchronized_pool_resource. Order-id
  /// lookup is a reserved @c std::unordered_map.
  ///
  /// Templated on @c Emitter so the same matching path serves both the production binary
  /// (where @c Emitter writes formatted lines to @c std::cout) and tests (where @c Emitter
  /// collects events into vectors). The emitter must provide @c operator() for
  /// @ref trade_event_t, @ref order_fully_filled_t, and @ref order_partially_filled_t.
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
    explicit clob_t(std::size_t capacity, Emitter emitter)
      : arena_(detail::clob_arena_byte_count(capacity)),
        mono_(arena_.data(), arena_.size(), std::pmr::new_delete_resource()),
        pool_(&mono_),
        order_arena_(detail::order_arena_byte_count(capacity)),
        order_mono_(order_arena_.data(), order_arena_.size(), std::pmr::new_delete_resource()),
        max_orders_(capacity),
        bids_(side_t::buy, &pool_),
        asks_(side_t::sell, &pool_),
        lookup_(),
        emitter_(std::move(emitter)) {
      lookup_.reserve(std::max<std::size_t>(capacity * 2, 16));
    }

    clob_t(const clob_t&) = delete;
    clob_t& operator=(const clob_t&) = delete;
    clob_t(clob_t&&) = delete;
    clob_t& operator=(clob_t&&) = delete;

    /// Process a parsed input event. The @c std::visit cost compiles down to a single
    /// branch on the variant index for our alternatives.
    void operator()(const input_event_t& event) {
      std::visit([this](const auto& concrete) { (*this)(concrete); }, event);
    }

    /// AddOrderRequest path: try to match the incoming order against the opposite side, then
    /// rest any residual quantity on its own side.
    void operator()(const add_order_request_t& event) {
      if (event.quantity <= 0) {
        return;  // No-op for non-positive quantities; the parser surfaces these as errors.
      }

      add_order_request_t residual = event;
      order_book_side_t& opposite = (event.side == side_t::buy) ? asks_ : bids_;
      match_against(opposite, residual);

      if (residual.quantity > 0) {
        rest_order(residual);
      }
    }

    /// CancelOrderRequest path: reserved @c std::unordered_map for ids; pooled @c pmr::map for prices.
    void operator()(const cancel_order_request_t& event) {
      const auto it = lookup_.find(event.order_id);
      if (it == lookup_.end() || !it->second->active) {
        // Cancel of an unknown / already-filled / already-cancelled id: no-op (no crash, no output).
        return;
      }
      order_node_t* node = it->second;
      order_book_side_t& side_book = (node->side == side_t::buy) ? bids_ : asks_;
      side_book.detach(node);
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
    [[nodiscard]] bool has_resting_capacity() const noexcept {
      return resting_alloc_count_ < max_orders_;
    }

    /// Allocates and value-initializes a node; bumps @ref resting_alloc_count_. Throws on OOM.
    [[nodiscard]] order_node_t* allocate_resting_node(order_id_t id, side_t side, price_t price, quantity_t qty) {
      void* const raw = order_mono_.allocate(sizeof(order_node_t), alignof(order_node_t));
      order_node_t* const node = static_cast<order_node_t*>(raw);
      std::construct_at(node);
      node->order_id = id;
      node->price = price;
      node->quantity = qty;
      node->side = side;
      node->active = true;
      node->prev_same_price = nullptr;
      node->next_same_price = nullptr;
      ++resting_alloc_count_;
      return node;
    }

    /// Walk price levels best-first, draining each level's intrusive chain until either the
    /// aggressive @p incoming is filled or the next level no longer crosses.
    void match_against(order_book_side_t& opposite, add_order_request_t& incoming) {
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
    void match_level(order_book_side_t& opposite, price_level_t& level, price_t level_price, add_order_request_t& incoming) {
      order_node_t* cursor = level.head;
      while (cursor != nullptr && incoming.quantity > 0) {
        order_node_t* const resting = cursor;
        order_node_t* const next = resting->next_same_price;

        if (!resting->active) {
          // Defensive: a retired order should already be unlinked, but the chain walk is
          // robust to a stale node either way.
          cursor = next;
          continue;
        }

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

    void retire_resting(order_book_side_t& opposite, price_level_t& level, order_node_t* node) noexcept {
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
      (void) opposite;
    }

    void rest_order(const add_order_request_t& residual) {
      if (!has_resting_capacity()) {
        // We are out of resting-order slots (constructor capacity). Drop silently rather than
        // crash; in production this would be surfaced via a dedicated diagnostic / backpressure.
        return;
      }
      order_node_t* const node =
        allocate_resting_node(residual.order_id, residual.side, residual.price, residual.quantity);
      if (!lookup_.try_emplace(residual.order_id, node).second) {
        // Duplicate id. Roll back the node we just claimed so a malformed request does not corrupt the book.
        node->active = false;
        return;
      }
      order_book_side_t& side_book = (residual.side == side_t::buy) ? bids_ : asks_;
      side_book.append(node);
    }

    std::vector<std::byte> arena_;
    std::pmr::monotonic_buffer_resource mono_;
    std::pmr::unsynchronized_pool_resource pool_;
    std::vector<std::byte> order_arena_;
    std::pmr::monotonic_buffer_resource order_mono_;
    std::size_t max_orders_{0};
    std::size_t resting_alloc_count_{0};
    order_book_side_t bids_;
    order_book_side_t asks_;
    std::unordered_map<order_id_t, order_node_t*> lookup_;
    Emitter emitter_;
  };

}  // namespace matching
