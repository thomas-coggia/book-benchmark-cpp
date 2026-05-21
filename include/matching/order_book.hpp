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
    quantity_t total_quantity{};
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
      level.total_quantity += order->quantity;

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

      it->second.total_quantity -= order->quantity;

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

    /// Sum of @ref price_level_t::total_quantity at/through @p limit_price, capped at @p need (FOK probe).
    [[nodiscard]] quantity_t fillable_quantity(price_t limit_price, quantity_t need) const noexcept {
      quantity_t available = 0;
      for (const auto& [price, level] : levels_) {
        if (!crosses(limit_price, price)) {
          break;
        }
        available += level.total_quantity;
        if (available >= need) {
          return need;
        }
      }
      return available;
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
  /// @tparam Emitter Must handle @ref trade_event_t and the terminal order output structs.
  ///
  /// Resting nodes live in a monotonic arena (@ref clob_memory_t). Ladders + id lookup share one
  /// PMR pool (best == @c begin()). For each @ref add_order_event_t the matcher walks the
  /// opposite ladder under price–time priority, accumulates fills into a single result, then
  /// emits one of @ref order_resting_event_t, @ref order_filled_event_t,
  /// @ref order_cancelled_event_t, or @ref order_rejected_event_t based on the order's @ref tif_t
  /// and what could actually be matched. @ref cancel_order_event_t produces a single
  /// @ref order_cancelled_event_t (cause @ref cancel_cause_t::user_request) or
  /// @ref order_rejected_event_t.
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

    /// Match opposite @ref order_book_side_t; emit exactly one terminal event for the add.
    void operator()(const add_order_event_t& event) {
      if (event.order_id <= 0) {
        emitter_(order_rejected_event_t{event.order_id, reject_code_t::invalid_order_id});
        return;
      }
      if (event.quantity <= 0) {
        emitter_(order_rejected_event_t{event.order_id, reject_code_t::invalid_quantity});
        return;
      }
      if (event.price <= 0) {
        emitter_(order_rejected_event_t{event.order_id, reject_code_t::invalid_price});
        return;
      }

      if (event.tif == tif_t::fok && !fillable_at_limit(event)) {
        emitter_(order_cancelled_event_t{event.order_id, quantity_t{0}, event.quantity, cancel_cause_t::fill_or_kill});
        return;
      }

      quantity_t filled_quantity{};
      add_order_event_t residual = event;
      if (event.side == side_t::buy) {
        match_against(asks_, residual, filled_quantity);
      } else {
        match_against(bids_, residual, filled_quantity);
      }

      const quantity_t residue = residual.quantity;
      if (residue == 0) {
        emitter_(order_filled_event_t{event.order_id, filled_quantity});
        return;
      }
      if (event.tif == tif_t::gtc) {
        if (!rest_order(residual)) {
          emitter_(order_rejected_event_t{event.order_id, reject_code_t::duplicate_order_id});
          return;
        }
        emitter_(order_resting_event_t{event.order_id, filled_quantity, residue});
        return;
      }
      const cancel_cause_t cause =
        event.tif == tif_t::ioc ? cancel_cause_t::immediate_or_cancel : cancel_cause_t::fill_or_kill;
      emitter_(order_cancelled_event_t{event.order_id, filled_quantity, residue, cause});
    }

    /// Cancel by id; emit exactly one terminal event (@c CAN on success, @c REJ otherwise).
    void operator()(const cancel_order_event_t& event) {
      if (event.order_id <= 0) {
        emitter_(order_rejected_event_t{event.order_id, reject_code_t::invalid_order_id});
        return;
      }
      const auto it = lookup_.find(event.order_id);
      if (it == lookup_.end() || !it->second->active) {
        emitter_(order_rejected_event_t{event.order_id, reject_code_t::unknown_order_id});
        return;
      }
      order_node_t* node = it->second;
      const quantity_t cancelled = node->quantity;
      if (node->side == side_t::buy) {
        bids_.detach(node);
      } else {
        asks_.detach(node);
      }
      node->active = false;
      lookup_.erase(it);
      emitter_(order_cancelled_event_t{event.order_id, quantity_t{0}, cancelled, cancel_cause_t::user_request});
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
    struct match_level_result_t {
      quantity_t filled_quantity{};
    };

    /// Allocate from @ref clob_memory_t::resting_order_resource (throws on OOM).
    [[nodiscard]] order_node_t* allocate_resting_node(order_id_t id, side_t side, price_t price, quantity_t qty) {
      void* const raw = memory_.resting_order_resource()->allocate(sizeof(order_node_t), alignof(order_node_t));
      return std::construct_at(
        static_cast<order_node_t*>(raw),
        order_node_t{
          .order_id = id,
          .price = price,
          .quantity = qty,
          .side = side,
          .active = true,
          .prev_same_price = nullptr,
          .next_same_price = nullptr,
        }
      );
    }

    /// True if crossed levels hold at least @c incoming.quantity in aggregate (@ref fillable_quantity probe).
    [[nodiscard]] bool fillable_at_limit(const add_order_event_t& incoming) const noexcept {
      const quantity_t need = incoming.quantity;
      const quantity_t available = incoming.side == side_t::buy ? asks_.fillable_quantity(incoming.price, need)
                                                                : bids_.fillable_quantity(incoming.price, need);
      return available >= need;
    }

    /// Walk opposite ladder best-first until @p incoming is flat or @ref order_book_side_t::crosses fails.
    template <side_t OppositeSide>
    void
    match_against(order_book_side_t<OppositeSide>& opposite, add_order_event_t& incoming, quantity_t& filled_quantity) {
      while (incoming.quantity > 0) {
        const auto best = opposite.best_level();
        if (!best) {
          return;
        }
        const auto [level_price, level] = *best;
        if (!opposite.crosses(incoming.price, level_price)) {
          return;
        }
        match_level_result_t level_result = match_level(opposite, *level, level_price, incoming);
        filled_quantity += level_result.filled_quantity;
        if (level->head == nullptr) {
          opposite.pop_best_level();
        }
      }
    }

    /// Drain one price level FIFO; @ref match_against erases the map level when empty.
    template <side_t OppositeSide>
    [[nodiscard]] match_level_result_t match_level(
      order_book_side_t<OppositeSide>& opposite,
      price_level_t& level,
      price_t level_price,
      add_order_event_t& incoming
    ) {
      quantity_t filled_quantity{};
      order_node_t* cursor = level.head;
      while (cursor != nullptr && incoming.quantity > 0) {
        order_node_t* const resting = cursor;
        order_node_t* const next = resting->next_same_price;

        assert(resting->active);

        const quantity_t resting_qty = resting->quantity;
        const quantity_t trade_qty = std::min(incoming.quantity, resting_qty);
        const quantity_t resting_remaining = resting_qty - trade_qty;

        emitter_(trade_event_t{incoming.order_id, resting->order_id, trade_qty});
        filled_quantity += trade_qty;
        level.total_quantity -= trade_qty;

        incoming.quantity -= trade_qty;

        if (resting_remaining == 0) {
          emitter_(order_filled_event_t{resting->order_id, trade_qty});
          retire_resting(opposite, level, resting);
        } else {
          resting->quantity = resting_remaining;
          emitter_(order_resting_event_t{resting->order_id, trade_qty, resting_remaining});
        }

        cursor = next;
      }
      return match_level_result_t{filled_quantity};
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

    [[nodiscard]] bool rest_order(const add_order_event_t& residual) {
      order_node_t* const node =
        allocate_resting_node(residual.order_id, residual.side, residual.price, residual.quantity);
      if (!lookup_.try_emplace(residual.order_id, node).second) {
        node->active = false;
        return false;
      }
      if (residual.side == side_t::buy) {
        bids_.append(node);
      } else {
        asks_.append(node);
      }
      return true;
    }

    clob_memory_t memory_;
    order_book_side_t<side_t::buy> bids_;
    order_book_side_t<side_t::sell> asks_;
    std::pmr::unordered_map<order_id_t, order_node_t*> lookup_;
    Emitter emitter_;
  };

}  // namespace matching
