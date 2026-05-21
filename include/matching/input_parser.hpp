#pragma once

#include <charconv>
#include <istream>
#include <optional>
#include <ostream>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

#include "matching/input_event.hpp"

namespace matching {

  namespace detail {

    /// Trim ASCII whitespace in-place.
    [[nodiscard]] inline std::string_view trim(std::string_view sv) noexcept {
      auto is_ws = [](char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
      while (!sv.empty() && is_ws(sv.front())) {
        sv.remove_prefix(1);
      }
      while (!sv.empty() && is_ws(sv.back())) {
        sv.remove_suffix(1);
      }
      return sv;
    }

    /// Next comma-separated field; trims; advances cursor.
    [[nodiscard]] inline std::string_view next_field(std::string_view& cursor) noexcept {
      const auto comma = cursor.find(',');
      std::string_view field;
      if (comma == std::string_view::npos) {
        field = cursor;
        cursor = std::string_view{};
      } else {
        field = cursor.substr(0, comma);
        cursor.remove_prefix(comma + 1);
      }
      return trim(field);
    }

    /// Strict integer parse (whole field must be digits/sign only).
    template <typename Int>
    [[nodiscard]] inline std::optional<Int> parse_int(std::string_view sv) noexcept {
      if (sv.empty()) {
        return std::nullopt;
      }
      Int value{};
      const auto* const begin = sv.data();
      const auto* const end = sv.data() + sv.size();
      const auto [ptr, ec] = std::from_chars(begin, end, value);
      if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
      }
      return value;
    }

    [[nodiscard]] inline std::optional<side_t> parse_side_token(std::string_view sv) noexcept {
      if (sv == wire_buy) {
        return side_t::buy;
      }
      if (sv == wire_sell) {
        return side_t::sell;
      }
      return std::nullopt;
    }

    [[nodiscard]] inline std::optional<tif_t> parse_tif_token(std::string_view sv) noexcept {
      if (sv == wire_tif_gtc) {
        return tif_t::gtc;
      }
      if (sv == wire_tif_ioc) {
        return tif_t::ioc;
      }
      if (sv == wire_tif_fok) {
        return tif_t::fok;
      }
      return std::nullopt;
    }

  }  // namespace detail

  /// Result of attempting to parse a single line.
  enum class parse_status_t : std::uint8_t {
    ok = 0,       ///< The line yielded an event (forwarded to the handler).
    skipped = 1,  ///< Empty / whitespace-only / comment line — no event, no diagnostic.
    error = 2,    ///< Ill-formed line — diagnostic emitted to the error sink.
  };

  /// Parse one line into @p out (@ref input_event_t).
  ///
  /// Only format-level validation is performed here: tag, field count, integer/side/TIF tokens.
  /// Value-bound checks (positive id/qty/price, id collision, unknown cancel id) are the
  /// matcher's responsibility — they produce @ref order_rejected_event_t on the output stream
  /// rather than diagnostics on @p err. Failures of pure shape go to @p err and the stream
  /// continues.
  [[nodiscard]] inline parse_status_t parse_line(std::string_view line, input_event_t& out, std::ostream& err) {
    const std::string_view trimmed = detail::trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      return parse_status_t::skipped;
    }

    std::string_view cursor = trimmed;
    const std::string_view opcode_field = detail::next_field(cursor);

    if (opcode_field == wire_add) {
      const auto order_id = detail::parse_int<order_id_t>(detail::next_field(cursor));
      const std::string_view side_token = detail::next_field(cursor);
      const auto quantity = detail::parse_int<quantity_t>(detail::next_field(cursor));
      const auto price = detail::parse_int<price_t>(detail::next_field(cursor));
      const std::string_view tif_token = detail::next_field(cursor);
      const auto side = detail::parse_side_token(side_token);
      const auto tif = detail::parse_tif_token(tif_token);
      if (!order_id || !quantity || !price || tif_token.empty() || !cursor.empty()) {
        std::println(err, "Ill-formed AddOrderRequest: {}", trimmed);
        return parse_status_t::error;
      }
      if (!side.has_value()) {
        std::println(err, "Invalid side token '{}' in AddOrderRequest: {}", side_token, trimmed);
        return parse_status_t::error;
      }
      if (!tif.has_value()) {
        std::println(err, "Invalid time-in-force token '{}' in AddOrderRequest: {}", tif_token, trimmed);
        return parse_status_t::error;
      }
      out = add_order_event_t{*order_id, *side, *quantity, *price, *tif};
      return parse_status_t::ok;
    }

    if (opcode_field == wire_cancel) {
      const auto order_id = detail::parse_int<order_id_t>(detail::next_field(cursor));
      if (!order_id || !cursor.empty()) {
        std::println(err, "Ill-formed CancelOrderRequest: {}", trimmed);
        return parse_status_t::error;
      }
      out = cancel_order_event_t{*order_id};
      return parse_status_t::ok;
    }

    std::println(err, "Unknown message type: {}", opcode_field);
    return parse_status_t::error;
  }

  /// Calls @ref parse_line per line; invokes handler on ok; writes errors to @p err.
  template <typename Handler>
  inline void parse_stream(std::istream& in, Handler&& handler, std::ostream& err) {
    std::string line;
    while (std::getline(in, line)) {
      input_event_t event{};
      const parse_status_t status = parse_line(line, event, err);
      if (status == parse_status_t::ok) {
        handler(event);
      }
    }
  }

}  // namespace matching
