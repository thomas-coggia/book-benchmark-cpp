# Limit-order matching engine

Single-symbol central limit order book: read **CSV-style lines from stdin**, match with **price–time priority**, write trades and fills to **stdout**. **Parse errors** go to **stderr**; the process keeps parsing (no crash on garbage).

This repo is a **deliberately improvable** sample: it avoids the last layers of proprietary tuning so nothing here reads as personal IP. The emphasis is **design**, **correctness**, and **maintainable code quality**, implemented as a **reasonable, STL-based** matching engine rather than a bleeding-edge exchange stack.

For diagrams, data structures, and complexity notes, see **[`ARCHITECTURE.md`](ARCHITECTURE.md)**. For what each benchmark preset represents, see **[`BENCHMARK.md`](BENCHMARK.md)**.

---

## Quick start

### 1. Build the Docker image

From the repository root:

```bash
docker build -t matching .
```

### 2. Start a container

```bash
docker run -it --rm --cap-add=SYS_NICE --ulimit rtprio=99 -v "$PWD":/app matching
```

The image uses `/app` as the working directory (your repo is mounted there).

### 3. Configure dependencies and compile (Release)

CMake presets extend Conan-generated toolchain presets ([Conan docs](https://docs.conan.io/2/examples/tools/cmake/cmake_toolchain/extend_own_cmake_presets.html)): root `CMakePresets.json` includes generated `ConanPresets.json`, which Conan refreshes on each `conan install`. Install once per preset layout you care about (same compiler settings; adjust `build_type`, `-o '&:matching_preset=…'`, and sanitizer preset name as needed):

```bash
conan profile detect --force
conan install . --build=missing \
  -s build_type=Release -s compiler=clang -s compiler.version=19 \
  -s compiler.cppstd=23 -s compiler.libcxx=libstdc++11 \
  -o '&:matching_preset=standard'
cmake --preset clang-19-release
cmake --build --preset clang-19-release
```

Sanitizer directories use `-o '&:matching_preset=asan'` (or `ubsan`, `tsan`) with `build_type=Debug`.

### 4. Run the tests

```bash
ctest --test-dir build/clang-19-release --output-on-failure
```

### 5. Run the engine on a recorded sample

```bash
./build/clang-19-release/bin/matching_engine < res/sample_1.stdin.txt > out.txt
```

### 6. Compare with recorded stdout

```bash
diff res/sample_1.stdout.txt out.txt
```

No diff means the run matches the golden file. `EnginePipelineTest.GoldenRecordedStdoutMatchesSampleFiles` checks the same under `ctest`.

### 7. Optional: benchmark binary

What each benchmark preset represents: **[`BENCHMARK.md`](BENCHMARK.md)**.

```bash
./build/clang-19-release/bin/benchmark --help
```

```bash
chrt 99 ./build/clang-19-release/bin/benchmark --orders 1000000 --iterations 10 --producer-cpu 2 --matcher-cpu 3 --stats-cpu 4
```

## Test System Configuration

Benchmark numbers (for example in [`benchmark.txt`](benchmark.txt)) were captured on a **consumer-grade laptop**; the setup below matches that machine so you can compare your own hardware to the quoted runs.

**Hardware:**
- Processor: Intel(R) Core(TM) i7-7700HQ CPU @ 2.80GHz (2.80 GHz)
- RAM: 32.0 GB
- Storage: 1.82 TB SSD
- System Type: 64-bit operating system, x64 processor

**Software:**
- Operating System: Linux (WSL2)
- Compiler: Clang 19+ with C++23 (CMake presets `clang-19-release` / `clang-19-debug`)

**Performance Expectations:**
- Consumer-grade laptop hardware, not server-grade exchange infrastructure.
- Typical scaling: noticeably higher throughput on server-class CPUs (Xeon/EPYC at higher clocks) is plausible; absolute factors depend on profile, pinning, and build flags.

---

## Wire format (message types)

Three-letter message tags (comma-separated fields; see `matching_engine --help`).

| Tag | Line shape | Meaning |
|-----|------------|---------|
| ADD | `ADD,id,side,qty,price` | Add order |
| CXL | `CXL,id` | Cancel |
| TRD | `TRD,qty,price` | Trade (output) |
| FFL | `FFL,id` | Fully filled (output) |
| PFL | `PFL,id,remaining` | Partially filled (output) |
| ERR | `ERR,id,kind` | Error (output); `kind` is numeric |

- Side must be **`BUY`** or **`SLL`** (exact spelling).
- Blank lines and lines starting with **`#`** are skipped.  
- Quantities and prices are integers (ticks) in this build.  
- Example recorded pair: `res/sample_1.stdin.txt` → `res/sample_1.stdout.txt`.

---

## Code map

| Topic | Main locations |
|------|----------------|
| Matching and book | `include/matching/order_book.hpp`, `clob_factory.hpp` |
| Input | `include/matching/input_parser.hpp` |
| Output lines | `include/matching/output_formatter.hpp` |
| Executable config | `include/app/matching_engine_config.hpp`, `include/app/benchmark_config.hpp` |
| Benchmark presets | [`BENCHMARK.md`](BENCHMARK.md), `include/matching/benchmark/market_profile.hpp` |
| Three-thread pipeline | `include/matching/runtime/*`, `src/matching_engine.cpp` |
| Tests | `test/` (46 GoogleTest cases), golden data in `res/` |

---

## Binaries

| Binary | Role |
|--------|------|
| `matching_engine` | stdin → reader → matcher → writer → stdout. Flags: `--capacity`, `--reader-cpu`, `--matcher-cpu`, `--writer-cpu`, `--help`. |
| `benchmark` | In-process load generator and latency stats; CLI flags (`--help`). |

---

## Other build flavors

**Debug:** `conan install` with `build_type=Debug` and `-o '&:matching_preset=standard'` (same compiler `-s` lines as Release), then presets `clang-19-debug`.

**Sanitisers:** `build_type=Debug` and `-o '&:matching_preset=asan'` / `ubsan` / `tsan`; binaries under `build/clang-19-<preset>/`. Example:

```bash
TSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build/clang-19-tsan --output-on-failure
```

**Fuzzer (optional):** `cmake -B build/fuzz -DBUILD_FUZZERS=ON` with a fuzz-capable toolchain; target `fuzz_input_parser` (see `test/fuzz_input_parser.cpp`).

---

## Repo notes

- **C++23**, Linux-oriented. **Conan 2** supplies **cxxopts** (CLI for `matching_engine` / `benchmark`) and **GoogleTest** (tests only). Library TUs stay third-party-free. **`cmake_layout()` + `ConanPresets.json`** ([extend presets](https://docs.conan.io/2/examples/tools/cmake/cmake_toolchain/extend_own_cmake_presets.html)): run `conan install` per variant so generators (including `cxxopts` CMake config) stay valid.
- Mostly **header-only**; the extra translation unit is `src/signal_handler.cpp` (global stop source for signals).
