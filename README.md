# Limit-order matching engine

Single-symbol central limit order book: read **CSV-style lines from stdin**, match with **price–time priority**, write trades and fills to **stdout**. **Parse errors** go to **stderr**; the process keeps parsing (no crash on garbage).

This repo is a **deliberately improvable** sample: it avoids the last layers of proprietary tuning so nothing here reads as personal IP. The emphasis is **design**, **correctness**, and **maintainable code quality**, implemented as a **reasonable, STL-based** matching engine rather than a bleeding-edge exchange stack.

On a **consumer-grade laptop**, the matcher sustains **~6M orders/second** at **~100 ns p50** latency across **five synthetic market regimes** (calm, active, quote-heavy, volatile, sweep). That is a fair baseline for an **STL-only** implementation, and the **consistency across regimes** matters as much as the headline number. Full data in [`benchmark.txt`](benchmark.txt).

For diagrams, data structures, and complexity notes, see [ARCHITECTURE.md](ARCHITECTURE.md). For what each benchmark preset represents, see [BENCHMARK.md](BENCHMARK.md).

---

## Quick start

### 1. Build the Docker image

From the repository root:

```bash
docker build -t matching .
```

### 2. Start a container

```bash
docker run -it --rm \
  --cap-add=SYS_NICE --ulimit rtprio=99 \
  --user "$(id -u):$(id -g)" \
  -v "$PWD":/app matching
  # add `--security-opt seccomp=unconfined` if building the TSan profile
```

`SYS_NICE`/`rtprio` let the benchmark pin and elevate threads; `--user` keeps `build/` host-owned.

### 3. Build & package

```bash
conan create . -pr:a=profiles/clang-19-release --build=missing
```

`conan create` runs the full lifecycle (configure → build → test → package) and lands the resulting `matching/0.1.0` package in the Conan cache. `--build=missing` lets Conan compile any dependency (e.g. `gtest`) Conan Center hasn't prebuilt for our clang-19 / libstdc++11 / C++23 combo. After the first run the cache is populated and subsequent builds skip straight to our code.

Other profiles in `profiles/`: `clang-19-debug`, `clang-19-asan`, `clang-19-ubsan`, `clang-19-tsan`.

### 4. Deploy into the workspace

```bash
conan install --requires=matching/0.1.0 -pr:a=profiles/clang-19-release \
              --deployer=direct_deploy --output-folder=install
```

Conan's `direct_deploy` extracts the package into `install/direct_deploy/matching/{bin,include,lib}` — a clean, FHS-shaped tree with no per-variant paths in the way.

### 5. Run the engine on a recorded sample

```bash
./install/direct_deploy/matching/bin/matching_engine < res/sample_1.stdin.txt > out.txt
cat out.txt
```

### 6. Compare with recorded stdout

```bash
diff res/sample_1.stdout.txt out.txt
```

No diff means the run matches the golden file. `EnginePipelineTest.GoldenRecordedStdoutMatchesSampleFiles` checks the same under `ctest`.

### 7. Optional: benchmark binary

What each benchmark preset represents: `**[BENCHMARK.md](BENCHMARK.md)**`.

```bash
./install/direct_deploy/matching/bin/benchmark --help
```

```bash
chrt 99 ./install/direct_deploy/matching/bin/benchmark --orders 1000000 --iterations 10 --producer-cpu 2 --matcher-cpu 3 --stats-cpu 4
```

## Test System Configuration

Benchmark numbers (for example in `[benchmark.txt](benchmark.txt)`) were captured on a **consumer-grade laptop**; the setup below matches that machine so you can compare your own hardware to the quoted runs.

**Hardware:**

- Processor: Intel(R) Core(TM) i7-7700HQ CPU @ 2.80GHz (2.80 GHz)
- RAM: 32.0 GB
- Storage: 1.82 TB SSD
- System Type: 64-bit operating system, x64 processor

**Software:**

- Operating System: Linux (WSL2)
- Compiler: Clang 19+ with C++23 (Conan-generated CMake presets `conan-release` / `conan-debug`)

**Performance Expectations:**

- Consumer-grade laptop hardware, not server-grade exchange infrastructure.
- Typical scaling: noticeably higher throughput on server-class CPUs (Xeon/EPYC at higher clocks) is plausible; absolute factors depend on profile, pinning, and build flags.

---

## Wire format (message types)

Three-letter message tags (comma-separated fields; see `matching_engine --help`).


| Tag | Line shape              | Meaning                           |
| --- | ----------------------- | --------------------------------- |
| ADD | `ADD,id,side,qty,price` | Add order                         |
| CXL | `CXL,id`                | Cancel                            |
| TRD | `TRD,qty,price`         | Trade (output)                    |
| FFL | `FFL,id`                | Fully filled (output)             |
| PFL | `PFL,id,remaining`      | Partially filled (output)         |
| ERR | `ERR,id,kind`           | Error (output); `kind` is numeric |


- Side must be `**BUY`** or `**SLL**` (exact spelling).
- Blank lines and lines starting with `**#**` are skipped.  
- Quantities and prices are integers (ticks) in this build.  
- Example recorded pair: `res/sample_1.stdin.txt` → `res/sample_1.stdout.txt`.

---

## Code map


| Topic                 | Main locations                                                                  |
| --------------------- | ------------------------------------------------------------------------------- |
| Matching and book     | `include/matching/order_book.hpp`, `clob_factory.hpp`                           |
| Input                 | `include/matching/input_parser.hpp`                                             |
| Output lines          | `include/matching/output_formatter.hpp`                                         |
| Executable config     | `include/app/matching_engine_config.hpp`, `include/app/benchmark_config.hpp`    |
| Benchmark presets     | `[BENCHMARK.md](BENCHMARK.md)`, `include/matching/benchmark/market_profile.hpp` |
| Three-thread pipeline | `include/matching/runtime/*`, `src/matching_engine.cpp`                         |
| Tests                 | `test/` (46 GoogleTest cases), golden data in `res/`                            |


---

## Binaries


| Binary            | Role                                                                                                                        |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `matching_engine` | stdin → reader → matcher → writer → stdout. Flags: `--capacity`, `--reader-cpu`, `--matcher-cpu`, `--writer-cpu`, `--help`. |
| `benchmark`       | In-process load generator and latency stats; CLI flags (`--help`).                                                          |


---

## Build details

**Dev iteration.** Skip `conan create` while iterating — `conan build` configures, builds, and lets you run tests in place without going through the full package lifecycle:

```bash
conan build . -pr:a=profiles/clang-19-release --build=missing
ctest --preset conan-release
./build/clang-19-release/bin/matching_engine < res/sample_1.stdin.txt
```

`CMakeToolchain` writes `CMakeUserPresets.json` at the repo root; CMake discovers it natively, so after any `conan build` you can rebuild incrementally without re-running Conan:

| Profile            | CMake preset name   |
| ------------------ | ------------------- |
| `clang-19-release` | `conan-release`     |
| `clang-19-debug`   | `conan-debug`       |
| `clang-19-asan`    | `conan-asan-debug`  |
| `clang-19-ubsan`   | `conan-ubsan-debug` |
| `clang-19-tsan`    | `conan-tsan-debug`  |

```bash
cmake --build --preset conan-release
ctest --preset conan-release
```

**Sanitisers.** Flags live in each profile's `[conf] tools.build:cxxflags+=[...]`, so Conan rebuilds `gtest` instrumented too — reports rooted in `gtest` internals are real, not false positives. TSan example with runtime options (also pass `--security-opt seccomp=unconfined` to `docker run`):

```bash
TSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --preset conan-tsan-debug
```

**Fuzzer (optional).** `cmake -B build/fuzz -DBUILD_FUZZERS=ON` with a fuzz-capable toolchain; target `fuzz_input_parser` (see `test/fuzz_input_parser.cpp`).

---

## Repo notes

- **C++23**, Linux-oriented. **Conan 2** supplies **cxxopts** (CLI for `matching_engine` / `benchmark`) and **GoogleTest** (tests only). Library TUs stay third-party-free. `conan create .` packages the project; `conan install --deployer=direct_deploy` extracts it into a clean `install/direct_deploy/matching/` tree. `conan build .` is the fast dev-iteration entry point. One profile per variant under `profiles/` (`clang-19-release`, `clang-19-debug`, `clang-19-{asan,ubsan,tsan}`); sanitisers live in profile `[conf]` and propagate to all dependencies. `CMakeToolchain` writes `CMakeUserPresets.json` for native `cmake --preset conan-…` rebuilds.
- Mostly **header-only**; the extra translation unit is `src/signal_handler.cpp` (global stop source for signals).

