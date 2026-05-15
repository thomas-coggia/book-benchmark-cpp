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

  /// Resting order at one price (intrusive FIFO); stable address until retired (@ref clob_t resting arena).
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

    /// Arena sizing for ladder + lookup (@c std::pmr::pool_resource nodes).
    [[nodiscard]] constexpr std::size_t ladder_lookup_arena_byte_count(std::size_t capacity) noexcept {
      constexpr std::size_t per_level = 96;
      constexpr std::size_t ceiling = 32 * 1024 * 1024;
      constexpr std::size_t floor = 262144;
      const std::size_t scaled =
        std::min<std::size_t>(std::max<std::size_t>(capacity, 16) * per_level, ceiling) + floor;
      return align_up(scaled, 4096);
    }

  }  // namespace detail

  /// Lookup allocator; shares @c ladder_lookup_pool with @ref order_book_side_t ladders.
  using ladder_lookup_allocator_t = std::pmr::polymorphic_allocator<std::pair<const order_id_t, order_node_t*>>;

  /// Intrusive FIFO at one resting price (one key in the side's ladder map).
  struct price_level_t {
    order_node_t* head{nullptr};
    order_node_t* tail{nullptr};
  };

  using order_book_level_allocator_t = std::pmr::polymorphic_allocator<std::pair<const price_t, price_level_t>>;

  /// Price ladder ordering for @c std::pmr::map: asks ascending, bids descending; best == @c begin().
  /// @ref order_book_side_t::crosses negates the same strict comparison on limits.
  template <side_t Side>
  using resting_levels_compare_t = std::conditional_t<Side == side_t::buy, std::greater<price_t>, std::less<price_t>>;

  /// One resting price ladder (@c std::pmr::map keys + intrusive FIFO per level).
  ///
  /// @tparam Side Resting side; selects @ref resting_levels_compare_t and @ref crosses().
  template <side_t Side>
  class order_book_side_t {
  public:
    explicit order_book_side_t(order_book_level_allocator_t alloc) : levels_(alloc) {}

    /// Crossing predicate for limits (@sa resting_levels_compare_t).
    [[nodiscard]] bool crosses(price_t aggressive_price, price_t resting_price) const noexcept {
      return !resting_levels_compare_t<Side>{}(aggressive_price, resting_price);
    }

    /// Enqueue at tail of order's price level (creates level if needed).
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

    /// Unlink from price level; erase empty level. Caller owns active / lookup cleanup.
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

    /// Best quote level (@c begin() under @ref resting_levels_compare_t), or @c std::nullopt.
    [[nodiscard]] std::optional<std::pair<price_t, price_level_t*>> best_level() noexcept {
      if (levels_.empty()) {
        return std::nullopt;
      }
      auto it = levels_.begin();
      return std::pair<price_t, price_level_t*>{it->first, &it->second};
    }

    /// Erase best-priced level (caller must have emptied its FIFO chain).
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

  /// PMR arenas for @ref clob_t (movable handle; impl keeps pool addresses stable).
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

    [[nodiscard]] std::size_t book_capacity() const noexcept {
      return storage_->book_capacity;
    }

    [[nodiscard]] order_book_level_allocator_t ladder_allocator() const noexcept {
      return order_book_level_allocator_t{&storage_->ladder_lookup_pool};
    }

    [[nodiscard]] ladder_lookup_allocator_t lookup_allocator() const noexcept {
      return ladder_lookup_allocator_t{&storage_->ladder_lookup_pool};
    }

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

  /// Single-symbol central limit book.
  ///
  /// @tparam Emitter Must handle @ref trade_event_t, fill structs, @ref order_error_event_t.
  ///
  /// Resting nodes: monotonic arena (@ref clob_memory_t). Ladders + id lookup share one PMR pool (best == @c begin()).
  /// Each fill emits trade → aggressive fill → resting fill; residue may rest without a fill line.
  template <typename Emitter>
  class clob_t {
  public:
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

    void operator()(const input_event_t& event) {
      std::visit([this](const auto& concrete) { (*this)(concrete); }, event);
    }

    /// Match opposite @ref order_book_side_t; rest residue. Bad fields → @ref order_error_event_t, no match.
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

    /// Cancel by id; missing or inactive → @ref order_error_event_t.
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

    /// No-op (@ref shutdown_t overload for @c std::visit).
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
    /// Allocate from @ref clob_memory_t::resting_order_resource (throws on OOM).
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

    /// Walk opposite ladder best-first until @p incoming is flat or @ref order_book_side_t::crosses fails.
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

    /// Drain one price level FIFO; @ref match_against erases the map level when empty.
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
      // match_against erases the map entry once head == nullptr.
      (void)opposite;
    }

    void rest_order(const add_order_event_t& residual) {
      order_node_t* const node =
        allocate_resting_node(residual.order_id, residual.side, residual.price, residual.quantity);
      if (!lookup_.try_emplace(residual.order_id, node).second) {
        // Duplicate id: discard allocated node (lookup unchanged).
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
