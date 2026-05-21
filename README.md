# Limit-order matching engine

Single-symbol central limit order book with **GTC, IOC, and FOK** time-in-force: read **CSV-style lines from stdin**, match with **price–time priority**, write trades and fills to **stdout**. Ill-formed input is set aside on **stderr**; processing continues gracefully.

This repo presents a **clear, shareable reference** where **design**, **correctness**, and **maintainable code quality** come first. The matching core is a **reasonable, STL-based** engine. It shows how far **software design** can take you on standard library building blocks, before diving into algorithmic optimizations. Venue-specific know-how is not published here.

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

`conan create` runs the full lifecycle (configure → build → test → package).

### 4. Deploy into the workspace

```bash
conan install --requires=matching/0.1.0 -pr:a=profiles/clang-19-release \
              --deployer=direct_deploy --output-folder=install
```

### 5. Run the engine on a recorded sample

```bash
./install/direct_deploy/matching/bin/matching_engine < res/sample_01.stdin.txt > out.txt
cat out.txt
```

### 6. Compare with recorded stdout

```bash
diff res/sample_01.stdout.txt out.txt
```

No diff means the run matches the golden file. `EnginePipelineTest.GoldenRecordedStdoutMatchesSampleFiles` checks the same under `ctest`.

### 7. Optional: benchmark binary

What each benchmark preset represents: **[BENCHMARK.md](BENCHMARK.md)**.

```bash
./install/direct_deploy/matching/bin/benchmark --help
```

```bash
chrt 99 ./install/direct_deploy/matching/bin/benchmark --orders 1000000 --iterations 10 --producer-cpu 2 --matcher-cpu 3 --stats-cpu 4
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
- Compiler: Clang 19+ with C++23 (Conan-generated CMake presets `conan-release` / `conan-debug`)

**Performance Expectations:**

- Consumer-grade laptop hardware, not server-grade exchange infrastructure.
- Typical scaling: noticeably higher throughput on server-class CPUs (Xeon/EPYC at higher clocks) is plausible; absolute factors depend on profile, pinning, and build flags.

---

## Wire format (message types)

Three-letter message tags, comma-separated fields, exactly one output line per matcher-visible input event (see `matching_engine --help`).

**Inputs**


| Tag | Line shape                  | Meaning                                                 |
| --- | --------------------------- | ------------------------------------------------------- |
| ADD | `ADD,id,side,qty,price,tif` | Add order. `side ∈ {BUY, SLL}`; `tif ∈ {GTC, IOC, FOK}` |
| CXL | `CXL,id`                    | Cancel a resting order                                  |


**Outputs** (terminal state per input; `filled + resting + cancelled == original_qty` for ADD)


| Tag | Line shape                         | Meaning                                        |
| --- | ---------------------------------- | ---------------------------------------------- |
| TRD | `TRD,aggressive-id,resting-id,qty` | Bilateral trade                                |
| RST | `RST,id,filled,resting`            | Accepted on the book; residue rests (GTC only) |
| FIL | `FIL,id,filled`                    | Fully filled                                   |
| CAN | `CAN,id,filled,cancelled,cause`    | Cancelled; `cause ∈ {USR, IOC, FOK}`           |
| REJ | `REJ,id,code`                      | Rejected; `code ∈ {DUP, UNK, IQT, IPR, IID}`   |


- Each matcher-visible input yields @c TRD rows per bilateral fill step, plus exactly one terminal row for the input order.
- Cancel causes: `USR` (explicit `CXL`), `IOC` (IOC residue), `FOK` (fill-or-kill could not fully execute).
- Reject codes: `DUP` (duplicate id), `UNK` (unknown cancel id or cancel of already-filled order), `IQT` (non-positive quantity), `IPR` (non-positive price), `IID` (non-positive id).
- Blank lines and lines starting with `#` are skipped.
- Quantities and prices are integers (ticks) in this build.
- Example recorded pairs: `res/sample_01.stdin.txt` … `res/sample_10.stdout.txt` (IOC/FOK, rejects, partial rests, parse errors).

---

## Code map


| Topic                 | Main locations                                                                  |
| --------------------- | ------------------------------------------------------------------------------- |
| Matching and book     | `include/matching/order_book.hpp`, `clob_factory.hpp`                           |
| Input                 | `include/matching/input_parser.hpp`                                             |
| Output lines          | `include/matching/output_formatter.hpp`                                         |
| Executable config     | `include/app/matching_engine_config.hpp`, `include/app/benchmark_config.hpp`    |
| Benchmark presets     | [`BENCHMARK.md`](BENCHMARK.md), `include/matching/benchmark/market_profile.hpp` |
| Three-thread pipeline | `include/matching/runtime/*`, `src/matching_engine.cpp`                         |
| Tests                 | `test/` (46 GoogleTest cases), golden data in `res/`                            |


---

## Binaries


| Binary            | Role                                                                                                                        |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `matching_engine` | stdin → reader → matcher → writer → stdout. Flags: `--capacity`, `--reader-cpu`, `--matcher-cpu`, `--writer-cpu`, `--help`. |
| `benchmark`       | In-process load generator and latency stats; CLI flags (`--help`).                                                          |


---

## Development

Day-to-day work skips the full package lifecycle from Quick start §3. Use `conan build` to configure and compile in the tree, then test and rebuild from CMake presets.

### 1. Configure and build

```bash
conan build . -pr:a=profiles/clang-19-release --build=missing
```

### 2. Test

```bash
ctest --preset conan-release
./build/clang-19-release/bin/matching_engine < res/sample_01.stdin.txt
```

### 3. Incremental rebuild

After the first `conan build`, rebuild without re-running Conan:

```bash
cmake --build --preset conan-release
ctest --preset conan-release
```

---

## Repo notes

- **Docker** for a consistent Linux build and benchmark environment on any host.
- **C++23**, Linux-oriented. **Conan 2** handles dependencies and the build lifecycle; **`profiles/`** for consistent, reproducible builds across environments.
- Mostly **header-only**; the extra translation unit is `src/signal_handler.cpp` (global stop source for signals).

