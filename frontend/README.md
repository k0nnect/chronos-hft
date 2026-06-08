# chronos-bt replay dashboard

a vite + react + typescript frontend that replays chronos-bt simulation traces:
order book depth, a realized p&l curve, an execution ticker tape & a transport bar
to scrub through the run. styled with tailwind in a dark theme.

## data format

the dashboard consumes the jsonl trace emitted by the c++ `trace_logger`. one json
object per line, one snapshot per logging interval:

```json
{"t":4000,"bb":1002046,"ba":1002052,"bids":[[1002046,29121],[1002045,61971]],"asks":[[1002052,36918],[1002053,78768]],"pos":-100,"pnl":2004.1040}
```

| field  | meaning                                            |
| ------ | -------------------------------------------------- |
| `t`    | simulation timestamp                               |
| `bb`   | best bid price in ticks (`null` if the side empty) |
| `ba`   | best ask price in ticks                            |
| `bids` | top levels inside-out, `[price, volume]`           |
| `asks` | top levels inside-out, `[price, volume]`           |
| `pos`  | signed position in shares                          |
| `pnl`  | realized p&l                                        |

execution prints are derived client-side from the change in `pos` between frames,
so the ticker tape stays in sync with the depth & p&l panels.

## generate a trace

build the c++ project & run the market-maker driver; it writes `trace.jsonl` into
the working directory:

```sh
./scripts/build.sh
cd build && ./micro_price_mm        # writes ./build/trace.jsonl
```

drop that file on the dashboard with **upload jsonl**, or copy it to
`frontend/public/sample-trace.jsonl` & use **load sample**.

## run the dashboard

```sh
cd frontend
npm install
npm run dev          # vite dev server on http://localhost:5173
```

`npm run build` produces a static bundle in `dist/`.

## layout

```
src/
  App.tsx                  page shell, state & layout
  types.ts                 frame schema, jsonl parser, print derivation
  format.ts                number / price / pnl formatters
  hooks/usePlayback.ts     frame-cursor transport (play / pause / speed / seek)
  components/
    DepthChart.tsx         cumulative bid/ask depth ladder
    PnlChart.tsx           realized p&l area curve (recharts)
    TickerTape.tsx         scrolling execution prints
    PlaybackController.tsx play / pause / speed / scrubber
    FileLoader.tsx         client-side .jsonl loader
    Panel.tsx              shared dark panel chrome
```
