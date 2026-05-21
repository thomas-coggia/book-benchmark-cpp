#pragma once

#include <cstdint>
#include <string_view>

namespace matching {

  /// Three-letter wire tags (newline-delimited CSV).
  ///
  /// Input opcodes: @ref wire_add (`ADD,<id>,<side>,<qty>,<price>,<tif>`) and @ref wire_cancel
  /// (`CXL,<id>`). Output: @ref wire_trade plus terminal order rows.
  inline constexpr std::string_view wire_add{"ADD"};
  inline constexpr std::string_view wire_cancel{"CXL"};
  inline constexpr std::string_view wire_trade{"TRD"};
  inline constexpr std::string_view wire_resting{"RST"};
  inline constexpr std::string_view wire_filled{"FIL"};
  inline constexpr std::string_view wire_cancelled{"CAN"};
  inline constexpr std::string_view wire_rejected{"REJ"};

  /// Side tokens on ADD lines.
  inline constexpr std::string_view wire_buy{"BUY"};
  inline constexpr std::string_view wire_sell{"SLL"};

  /// Time-in-force tokens on ADD lines.
  inline constexpr std::string_view wire_tif_gtc{"GTC"};
  inline constexpr std::string_view wire_tif_ioc{"IOC"};
  inline constexpr std::string_view wire_tif_fok{"FOK"};

  /// Cancel-cause tokens on @ref wire_cancelled lines.
  inline constexpr std::string_view wire_cause_user{"USR"};
  inline constexpr std::string_view wire_cause_ioc{"IOC"};
  inline constexpr std::string_view wire_cause_fok{"FOK"};

  /// Reject-code tokens on @ref wire_rejected lines.
  inline constexpr std::string_view wire_reject_duplicate_id{"DUP"};
  inline constexpr std::string_view wire_reject_unknown_id{"UNK"};
  inline constexpr std::string_view wire_reject_invalid_quantity{"IQT"};
  inline constexpr std::string_view wire_reject_invalid_price{"IPR"};
  inline constexpr std::string_view wire_reject_invalid_order_id{"IID"};

  /// Order side. Wire uses @c BUY and @c SLL on add lines (buy / sell).
  enum class side_t : std::uint8_t {
    buy = 0,
    sell = 1
  };

  /// Time-in-force for an add order.
  ///
  /// @c gtc  — Good-till-cancel: residue after matching rests on the book indefinitely.
  /// @c ioc  — Immediate-or-cancel: match what is possible immediately; any residue is cancelled.
  /// @c fok  — Fill-or-kill: the order is filled entirely at the limit price or wholly cancelled
  ///           with no fills.
  enum class tif_t : std::uint8_t {
    gtc = 0,
    ioc = 1,
    fok = 2
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
