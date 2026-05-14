#pragma once

#include <cstddef>
#include <utility>

#include "matching/order_book.hpp"

namespace matching {

  /// Concrete factory that constructs a @ref clob_t with a pre-decided capacity and emitter.
  /// Shared by the `matching_engine` and `benchmark` binaries so both use the same construction
  /// path; the only template parameter is the static @c Emitter type.
  ///
  /// @c create() is a one-shot prvalue producer; mandatory copy elision (C++17) lets us
  /// hand back a non-movable @ref clob_t directly.
  template <typename Emitter>
  class clob_factory_t {
  public:
    using emitter_type = Emitter;
    using book_type = clob_t<Emitter>;

    constexpr clob_factory_t(std::size_t capacity, Emitter emitter) noexcept
      : capacity_(capacity), emitter_(std::move(emitter)) {}

    /// Build a fresh @ref clob_t configured with the captured capacity and emitter. The
    /// emitter is move-consumed so the factory is single-shot by intent.
    [[nodiscard]] book_type create() && {
      return book_type{capacity_, std::move(emitter_)};
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
      return capacity_;
    }

  private:
    std::size_t capacity_{};
    Emitter emitter_;
  };

  /// CTAD helper: deduces the @c Emitter from the constructor argument so callers can write
  /// @c clob_factory{capacity, std::move(my_emitter)}.
  template <typename Emitter>
  clob_factory_t(std::size_t, Emitter) -> clob_factory_t<Emitter>;

}  // namespace matching
