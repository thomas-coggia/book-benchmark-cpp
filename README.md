# Limit-order matching engine

Single-symbol central limit order book: read **CSV-style lines from stdin**, match with **price–time priority**, write trades and fills to **stdout**. Invalid lines go to **stderr**; the process keeps parsing (no crash on garbage).

For diagrams, data structures, and complexity notes, see **[`ARCHITECTURE.md`](ARCHITECTURE.md)**.

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

### 5. Run the engine on the sample file

```bash
./build/clang-19-release/bin/matching_engine < res/sample_input.txt > out.txt
```

### 6. Compare with the expected output

```bash
diff res/sample_output.txt out.txt
```

No diff output means the sample matches.

### 7. Optional: benchmark binary

```bash
./build/clang-19-release/bin/benchmark --help
```

```bash
chrt 99 ./build/clang-19-release/bin/benchmark --orders 1000000 --iterations 3 --producer-cpu 2 --matcher-cpu 3 --stats-cpu 4
```

## Test System Configuration

Bench runs (for example captures in [`benchmark.txt`](benchmark.txt)) use the setup below so you can compare your machine to the quoted numbers.

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

| Msg | Line shape | Meaning |
|-----|------------|---------|
| 0 | `0,id,side,qty,price` | Add order |
| 1 | `1,id` | Cancel |
| 2 | `2,qty,price` | Trade (output) |
| 3 | `3,id` | Fully filled (output) |
| 4 | `4,id,remaining` | Partially filled (output) |

- `side`: **0** = buy, **1** = sell.  
- Blank lines and lines starting with **`#`** are skipped.  
- Quantities and prices are integers (ticks) in this build.  
- Full example: `res/sample_input.txt` → `res/sample_output.txt`.

---

## Code map

| Topic | Main locations |
|------|----------------|
| Matching and book | `include/matching/order_book.hpp`, `clob_factory.hpp` |
| Input | `include/matching/input_parser.hpp` |
| Output lines | `include/matching/output_formatter.hpp` |
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

- **C++23**, Linux-oriented. **Conan 2** supplies GoogleTest only; no runtime third-party deps in the engine path. **`cmake_layout()` + `ConanPresets.json`** ([extend presets](https://docs.conan.io/2/examples/tools/cmake/cmake_toolchain/extend_own_cmake_presets.html)): run `conan install` per variant so includes stay valid.
- Mostly **header-only**; the extra translation unit is `src/signal_handler.cpp` (global stop source for signals).
