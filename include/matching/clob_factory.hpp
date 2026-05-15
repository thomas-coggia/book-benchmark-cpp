#pragma once

#include <cstddef>
#include <utility>

#include "matching/order_book.hpp"

namespace matching {

  /// Builds @ref clob_t (@ref clob_memory_t + emitter); @c create() consumes emitter (NRVO-friendly).
  template <typename Emitter>
  class clob_factory_t {
  public:
    using emitter_type = Emitter;
    using book_type = clob_t<Emitter>;

    constexpr clob_factory_t(std::size_t capacity, Emitter emitter) noexcept
      : capacity_(capacity), emitter_(std::move(emitter)) {}

    [[nodiscard]] book_type create() && {
      return book_type{clob_memory_t{capacity_}, std::move(emitter_)};
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
      return capacity_;
    }

  private:
    std::size_t capacity_{};
    Emitter emitter_;
  };

  /// Deduces Emitter from constructor argument.
  template <typename Emitter>
  clob_factory_t(std::size_t, Emitter) -> clob_factory_t<Emitter>;

}  // namespace matching
