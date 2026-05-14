// libFuzzer entry point for @ref matching::input_parser. Built only when the toolchain
// supports @c -fsanitize=fuzzer (controlled via the @c BUILD_FUZZERS CMake option).
//
// The parser is supposed to be totally robust: any byte sequence on stdin must either yield
// an event (handed to a no-op handler here) or be reported on stderr as a parse error.
// Feeds a @c std::istringstream so behavior matches the production stream parser.
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "matching/input_event.hpp"
#include "matching/input_parser.hpp"

namespace {

  /// Discard every event the parser surfaces; we only care about whether the parser crashes
  /// or asserts.
  struct sink_t {
    void operator()(const matching::input_event_t&) const noexcept {}
  };

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string input{reinterpret_cast<const char*>(data), size};
  std::istringstream in{input};
  std::ofstream devnull{"/dev/null"};
  if (!devnull) {
    return 0;
  }
  matching::parse_stream(in, sink_t{}, devnull);
  return 0;
}
