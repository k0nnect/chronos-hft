# hft-bt — hft backtester with hardware acceleration

an ultra-low-latency high-frequency trading backtester built around a
cache-friendly limit order book in c++20, with a simulated fpga feature-extraction
pipeline described in systemverilog and bridged to the software engine over a
modelled axi-stream / pcie interface.

## status

| phase | scope | state |
|------:|-------|-------|
| 1 | core data structures + software l3 order book | done |
| 2 | binary feed handler (itch-like) + spsc ring buffer | done |
| 3 | backtest engine, strategy api, fill simulation | done |
| 4 | systemverilog feature engine + verilator cosim bridge | done |

## layout

```
include/hft/        public headers (header-only core)
  core/             fixed-width types, cache helpers, compiler intrinsics,
                    byte-order helpers, memory pool
  book/             price level, order-id index map, l3 order book, event apply
  feed/             wire protocol, zero-copy parser, spsc ring, feed handler,
                    synthetic market generator
  engine/           book-update snapshot, crtp strategy + order gateway,
                    queue-aware fill model, metrics, backtest engine
  strategies/       sample strategies (micro-price market maker)
hardware/           systemverilog accelerator + verilator cosim
  rtl/              frac divider, micro-price, volume-imbalance, feature_extractor,
                    axi-stream interface
  dpi/              fixed-point reference + verilated-model c++ wrapper
  tb/               systemverilog testbench + c++ cosim cross-check
src/                executables / translation units
hardware/           (phase 4) systemverilog rtl, testbenches, dpi bridge
  rtl/  tb/  dpi/
tests/              assert-based unit tests (no external framework)
bench/              standalone micro-benchmarks
cmake/              shared compiler/optimisation flags
scripts/            build helpers
data/               captured / synthetic market data
```

## the order book

phase 1 implements an l3 (per-order) book optimised for the hot path:

- **prices are integer ticks.** each side owns a flat, directly-indexed array of
  price levels over a fixed band, so tick → level is one subtraction.
- **resting orders live in a pre-reserved object pool** and are threaded into
  per-level fifo queues with intrusive 32-bit links, preserving time priority.
- **order-id → slot lookup** is an open-addressing flat map; cancels and
  executes are O(1).
- **best bid/ask are cached indices**, re-scanned over contiguous memory only
  when a top level empties.
- **zero heap allocation, no locks and no exceptions on add / cancel / execute.**

analytics computed directly from level aggregates: spread, mid, size-weighted
micro-price, and multi-level order-flow imbalance — the same quantities the
phase 4 fpga engine will compute in hardware.

## the feed pipeline

phase 2 turns a raw binary market-data stream into book mutations:

- **wire protocol** — an itch-like binary format: a 2-byte big-endian length
  prefix per frame, then a fixed-width big-endian message body (add / execute /
  cancel / delete / replace / trade). packed structs pin the on-wire sizes.
- **zero-copy parser** — `frame_cursor` walks the buffer handing back body
  pointers without copying; `decode` reads fields with `load_be` straight into a
  normalized `market_event`. alignment-safe, strict-aliasing-safe, and resilient
  to a truncated trailing frame (it stops and reports bytes consumed).
- **lock-free spsc ring** — wait-free single-producer/single-consumer queue with
  head/tail on separate cache lines and per-side cached indices, so the common
  case never reads the contended atomic. it is the wire between the feed thread
  and the engine thread.
- **feed handler** — drives bytes → events → a sink (a vector in tests, the ring
  in the live path), tracking parsed / malformed / consumed counters.
- **synthetic generator** — produces a deterministic, internally-consistent itch
  stream and reports the exact resting-order count the book must hold after
  replay, which the integration test asserts against.

representative single-machine throughput (release, `-O3 -march=native`): decode
~3.5 ns/msg (~8 GB/s), cross-thread ring hop ~25 ns, full feed→ring→book pipeline
~70 ns/event. run `./build/bench_feed` to reproduce.

## the backtest engine

phase 3 is the trading loop: events become book mutations, simulated fills,
strategy decisions and metrics, with no virtual dispatch and no hot-path
allocation.

- **crtp strategy** — strategies derive from `strategy<Derived>`; the engine
  dispatches `on_market_event` / `on_book_update` / `on_order_fill` through a
  `static_cast`, never a vtable. orders flow back through a concrete
  `order_gateway` (a fixed staging buffer the engine drains each tick), so the
  strategy stays decoupled from the book/fill-model types.
- **queue-position-aware fill model** — our orders are not inserted into the
  feed-driven book (that would corrupt replay); instead each working order tracks
  the real volume resting ahead of it and only fills once that queue trades out.
  models tick-to-trade latency, partial fills, marketable/market taker sweeps
  across levels, and maker/taker fees (negative maker bp = rebate).
- **metrics engine** — O(1) per update, no trade history: signed average-cost
  realized/unrealized p&l, fees, position, peak inventory, max drawdown, and a
  welford accumulator for a sharpe estimate.
- **backtest engine** — resolves each event's queue-consumption signal from the
  pre-mutation book, applies it, ticks the fill model, delivers fills to strategy
  and metrics, hands the strategy a fresh top-of-book snapshot, and marks to
  market — all templated on the concrete book and strategy.
- **sample strategy** — a micro-price market maker that joins the inside, leans
  on micro-price/imbalance to withdraw from the side about to be run over, and
  respects a hard inventory cap. run `./build/micro_price_mm`.

## the hardware accelerator

phase 4 offloads feature extraction (micro-price + volume imbalance) to a
simulated fpga described in synthesizable systemverilog and co-simulated with
verilator.

- **fixed-point datapath** — micro-price is computed as
  `bid + (ask - bid) * weight` with `weight = bid_qty / (bid_qty + ask_qty)`, a
  proper fraction produced by a pipelined **radix-2 shift/subtract fractional
  divider** (no inferred `/`). imbalance reuses the same divider on
  `|bid_qty - ask_qty| / (bid_qty + ask_qty)` with the sign reapplied. both
  datapaths share a FRAC+3 cycle latency, are fully pipelined (one result/cycle),
  use strictly non-blocking sequential logic, and carry the `valid` bit through
  every stage.
- **axi4-stream** — a 128-bit inbound book-update beat and a 64-bit outbound
  feature beat, with a standard `tvalid`/`tready`/`tlast` interface.
- **bit-exact reference** — `hardware/dpi/feature_reference.hpp` is the single
  source of truth for the fixed-point math; the rtl mirrors it exactly. it is
  validated against the floating-point `order_book` in `test_feature_golden`
  (no verilator needed): across ~200k two-sided states the worst error is 3 ulp
  (micro-price) and 1 ulp (imbalance) of Q16.16.
- **cosimulation** — `FpgaFeatureEngine` wraps the verilated model and drives its
  clock, modelling the pipeline latency. `hardware/tb/cosim_check.cpp` streams the
  phase-2 feed through the rtl and asserts `rtl == reference` bit-for-bit and
  `reference == order_book` within the ulp window. a systemverilog self-checking
  testbench (`hardware/tb/tb_feature_extractor.sv`) covers the rtl standalone.

### building the hardware layer

requires [verilator](https://verilator.org) (5.x recommended). the rest of the
project builds and tests without it.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHFT_BUILD_HARDWARE=ON
cmake --build build -j
ctest --test-dir build -R hardware_cosim --output-on-failure   # rtl vs reference
cmake --build build --target hardware_lint                     # verilator --lint-only
./build/bench_hardware                                         # sw vs hw comparison

# run the pure-systemverilog testbench directly:
verilator --binary --timing -Wall --top-module tb_feature_extractor \
  hardware/rtl/axi_stream_if.sv hardware/rtl/frac_divider.sv \
  hardware/rtl/micro_price.sv hardware/rtl/volume_imbalance.sv \
  hardware/rtl/feature_extractor.sv hardware/tb/tb_feature_extractor.sv
./obj_dir/Vtb_feature_extractor
```

## build

requires cmake ≥ 3.20 and a c++20 compiler (gcc 11+, clang 13+, or msvc 19.3+).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/hft_demo
```

`scripts/build.sh` wraps the same steps.

release builds compile with `-O3 -march=native -mtune=native`, link-time
optimisation, and aggressive scalar/vector flags (see `cmake/compilerflags.cmake`).
