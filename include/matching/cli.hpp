#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace matching {

  /// Tiny hand-rolled argv parser. Supports two flag forms:
  ///   * @c --key value       (two argv slots)
  ///   * @c --key=value       (one argv slot)
  /// And a third for booleans:
  ///   * @c --key              (presence-only flag, value defaults to @c "true")
  ///
  /// Positional arguments (anything not starting with @c "--") are collected into
  /// @ref positional in input order. Unknown keys are simply preserved — callers consult
  /// the parser by name and decide policy themselves; this keeps the parser dependency-free
  /// and keeps validation in the binary that owns the schema.
  class cli_args_t {
  public:
    explicit cli_args_t(int argc, char** argv) {
      for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg.starts_with("--")) {
          arg.remove_prefix(2);
          const auto eq = arg.find('=');
          if (eq != std::string_view::npos) {
            entries_.push_back({std::string{arg.substr(0, eq)}, std::string{arg.substr(eq + 1)}});
            continue;
          }
          // No '=': peek the next argv to see if it's a value (not another --flag).
          if (i + 1 < argc) {
            std::string_view peek{argv[i + 1]};
            if (!peek.starts_with("--")) {
              entries_.push_back({std::string{arg}, std::string{peek}});
              ++i;
              continue;
            }
          }
          entries_.push_back({std::string{arg}, std::string{"true"}});
        } else {
          positional_.emplace_back(arg);
        }
      }
    }

    /// Looks up the most recently provided value for @p key. Returns @c std::nullopt when
    /// the flag was not set on the command line.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view key) const noexcept {
      for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->key == key) {
          return std::string_view{it->value};
        }
      }
      return std::nullopt;
    }

    /// True when @p key was given without a paired value (e.g. @c --help). Implemented as
    /// "value resolves to literal @c true", matching the constructor's encoding.
    [[nodiscard]] bool has_flag(std::string_view key) const noexcept {
      const auto value = get(key);
      return value.has_value() && *value == "true";
    }

    [[nodiscard]] const std::vector<std::string>& positional() const noexcept {
      return positional_;
    }

  private:
    struct entry_t {
      std::string key;
      std::string value;
    };
    std::vector<entry_t> entries_{};
    std::vector<std::string> positional_{};
  };

}  // namespace matching
