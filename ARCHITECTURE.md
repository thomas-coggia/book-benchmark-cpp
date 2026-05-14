# Architecture

Companion to [`README.md`](README.md): how the process is structured end-to-end, what runs on which thread, and how the book and queues behave.

---

## System overview

The **matching_engine** binary runs a **three-agent pipeline**: a reader pulls text lines from **stdin**, parses them into **input events**, a matcher owns a **header-only CLOB** (`clob_t`) and produces **output events**, and a writer formats lines to **stdout**. **SPSC queues** connect the stages. Parse errors are reported on **stderr**; matching continues. **SIGINT** / **SIGTERM** cooperate with `std::stop_source` so the threads can exit cleanly.

---

## Architecture diagram

```
┌──────────┐
│  stdin   │
└────┬─────┘
     │ lines (CSV msgtype)
     ▼
┌─────────────────┐     ┌──────────────┐     ┌─────────────────┐
│  Reader agent   │────▶│ Input queue  │────▶│  Matcher agent  │
│  (thread 0)*    │     │   (SPSC)      │     │  (thread 1)*     │
│                 │     │               │     │                 │
│  · read line    │     │ input_event_t │     │  · clob_t match │
│  · parse        │     │               │     │  · enqueue outs │
└────────┬────────┘     └──────────────┘     └────────┬────────┘
         │                                            │
         │ malformed ────────────────────────────────┼──▶ stderr
         │                                            │
         │                                            ▼
         │                                   ┌──────────────┐
         │                                   │ Output queue │
         │                                   │   (SPSC)      │
         │                                   └──────┬───────┘
         │                                          │
         │                                          │ output_event_t
         │                                          ▼
         │                                   ┌──────────────┐
         │                                   │ Writer agent │
         │                                   │ (thread 2)*  │
         │                                   │              │
         │                                   │ · format line│
         └── shutdown_t / stop token ────────┤ · stdout     │
                                             └──────────────┘
```

\*Threads can optionally be pinned to CPUs via CLI flags (`--reader-cpu`, `--matcher-cpu`, `--writer-cpu`).

---

## Event flow

```
Text line
    → input_parser (bounded buffer)
    → input_event_t variant
    → input SPSC queue
    → clob_t<Emitter> (match / cancel / rest)
    → output_event_t variant (trades, fills, …)
    → output SPSC queue
    → output_formatter
    → text line on stdout
```

**Variants (conceptually):**

```cpp
input_event_t  = variant<add_order_request_t, cancel_order_request_t, shutdown_t>;
output_event_t = variant<trade_event_t, order_fully_filled_t, order_partially_filled_t, shutdown_t>;
```

`shutdown_t` is **in-band** for the pipeline only; it never appears on the wire. The production **Emitter** pushes `output_event_t` to the writer queue; the **benchmark** path swaps in an emitter that only counts trades.

---

## Three-agent pipeline

1. **Reader (producer to input queue)**  
   - Reads stdin line-by-line.  
   - Uses `input_parser` / `parse_stream`; errors to **stderr**, continue.  
   - Pushes `add_order_request_t`, `cancel_order_request_t`, or forwards **`shutdown_t`** after EOF.

2. **Matcher (consumer + producer)**  
   - Pops `input_event_t` from the input SPSC.  
   - Runs `clob_t` logic: resting orders, cancel-by-id, matching with **price then time**.  
   - Emits **`trade_event_t` first**, then aggressive fill, then resting fill (`order_fully_filled_t` / `order_partially_filled_t`) in that order (see wire format in README).  
   - Forwards `shutdown_t` to the output side when appropriate.

3. **Writer (consumer from output queue)**  
   - Pops `output_event_t`, formats with `output_formatter`, writes to **stdout**.  
   - Drains after shutdown so queues empty in a defined order (**join** reverses topology: writer → matcher → reader).

**Orchestration:** `agent_system_t` + `event_loop_t`; one global `stop_source` from `signal_handler.cpp` for signal-driven abort.

---

## Project structure

```
test-7cc798/
├── include/matching/
│   ├── order_book.hpp          # CLOB + matching
│   ├── clob_factory.hpp
│   ├── input_parser.hpp
│   ├── input_event.hpp
│   ├── output_event.hpp
│   ├── output_formatter.hpp
│   ├── message_types.hpp
│   ├── cli.hpp
│   ├── compile.cpp             # umbrella TU for clangd/tidy (includes all public headers)
│   ├── runtime/
│   │   ├── agent.hpp
│   │   ├── agent_system.hpp
│   │   ├── event_loop.hpp
│   │   ├── spsc_queue.hpp
│   │   └── signal_handler.hpp
│   └── benchmark/              # load generator, stats, timers
├── src/
│   ├── matching_engine.cpp
│   ├── benchmark.cpp
│   └── signal_handler.cpp      # only TU for global stop source
├── test/                       # GoogleTest
│   ├── test_order_book.cpp
│   ├── test_input_parser.cpp
│   ├── test_spsc_queue.cpp
│   ├── test_agent.cpp
│   ├── test_engine_pipeline.cpp
│   └── …
└── res/                        # golden stdin/stdout samples
```

---

## Component details

### Order book (`clob_t<Emitter>`)

| Piece | Role |
|-------|------|
| `order_node_t` | Resting order record (`alignas(64)`), bump-allocated, stable pointers. |
| `order_book_side_t` | `std::pmr::map<price, level>`; each level is an **intrusive FIFO** list of nodes. |
| Cancel map | Open addressing + linear probing (SplitMix64): **O(1)** id → node. |

**Complexity (informal):**  
- **Does an add cross?** **O(1)** to inspect best opposite level; then walk fills.  
- **Remove filled resting orders:** **O(1)** unlink + hash erase; when a level goes empty, **O(log L)** map erase (L = active price levels on that side).  
- **Cancel by id:** **O(1)** hash + unlink + possible **O(log L)** if level empties.  
- Residual resting insert: **O(log L)** map + **O(1)** node. Multi-level sweep: **O(M)** for M fills.

**Why this shape:** PMR-backed maps for allocator locality; intrusive lists avoid per-order heap traffic on the hot matching path; open addressing keeps cancel lookup predictable.

### SPSC queue (`runtime/spsc_queue.hpp`)

- Single producer, single consumer; **no mutex**.  
- Power-of-two capacity, **release/acquire** on head/tail, cache-line padded indices.  
- **try_push** / **try_pop**; full queue ⇒ spin with **cpu_pause** and stop-token checks (no implicit backpressure policy).

### Input path

- Newline-delimited lines via `std::getline`; streaming `parse_stream` on `std::istream` (stdin in the binary).  
- Strict integer message-type prefixes; malformed lines → error on the configured `std::ostream` (stderr in the binary), stream continues.

### Output path

- `output_formatter` maps each `output_event_t` alternative to one line on its bound `std::ostream` (stdout in the binary).  
- Ordering of emissions follows the book’s match routine (trade before fills).

### Benchmark path (same book, different I/O)

- Prebuilt `input_event_t` buffer → producer **thread** → matcher loop → stats (e.g. Welford / reservoir) on a side channel — no UDP and no stdin in that mode.

---

## Performance characteristics

| Topic | Notes |
|-------|--------|
| **Hot path** | Intrusive lists + hash cancel + map for sparse price levels; tuned for single-symbol throughput. |
| **Churn** | Heavy create/destroy of price levels (e.g. volatile synthetic profiles) stresses **log L** on `pmr::map`; other level containers would trade constants vs. asymptotics. |
| **Concurrency** | Book accessed from **one** thread (matcher); queues isolate other threads. |
| **I/O** | Line-at-a-time stdio; production might batch (vectored I/O, ring buffers). |

CPU affinity is optional (CLI pins); benchmark uses the same idea for apples-to-apples latency.

---

## Thread safety

- **SPSC:** safe for exactly one producer and one consumer per queue instance.  
- **CLOB:** single-threaded (matcher only).  
- **Formatter / stdout:** single writer thread.  

**No mutexes** on the steady-state matching path between agents; synchronization is via the queues and atomics inside them.

---

## Shutdown and errors

| Mode | Behaviour |
|------|------------|
| **Graceful** | Reader sends **`shutdown_t`** after EOF; stages forward it; threads exit via handler return value / sentinel. |
| **Signal / abort** | `request_stop()` on global source; loops observe token; in-flight work may be dropped. |
| **Parse errors** | Log-style output on **stderr**; input queue still receives valid events. |

---

## Wire format

Same as [`README.md`](README.md) (**msgtype** table). Integers on the wire; comments and blank lines ignored in input.
