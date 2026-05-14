#pragma once

#include <cstdint>

namespace matching {

  /// Order side. Values match the wire format encoding (0 = Buy, 1 = Sell).
  enum class side_t : std::uint8_t {
    buy = 0,
    sell = 1
  };

  /// Unique order identifier from the input stream. Valid input uses unique positive
  /// integers; we keep it signed to round-trip cleanly through formatting.
  using order_id_t = std::int64_t;

  /// Price in raw integer units. Other formats might use fractional prices; this engine and
  /// the bundled samples quantise on a tick grid, so we work in integer ticks.
  using price_t = std::int64_t;

  /// Order quantity. Always strictly positive in valid input.
  using quantity_t = std::int64_t;

  /// In-band end-of-pipeline sentinel. Travels through input/output queues and auxiliary
  /// benchmark queues so each consumer observes "no more events on this channel; finish and
  /// exit". Never serialised on the wire.
  struct shutdown_t {};

}  // namespace matching
