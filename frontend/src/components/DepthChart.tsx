import { Panel } from './Panel'
import type { Frame } from '../types'
import { fmtInt } from '../format'

// cumulative volume ladder over the top-N levels of each side. asks are stacked
// above the spread (far price at the top, best ask just above it); bids below
// (best bid just under the spread, far price at the bottom). each bar's width is
// the cumulative volume from the touch outward, scaled to the deepest side.
export function DepthChart({ frame }: { frame: Frame | undefined }) {
  if (!frame) {
    return (
      <Panel title="order book depth">
        <Empty />
      </Panel>
    )
  }

  const asks = cumulate(frame.asks)
  const bids = cumulate(frame.bids)
  const maxCum = Math.max(1, ...asks.map((l) => l.cum), ...bids.map((l) => l.cum))
  const spread = frame.bb != null && frame.ba != null ? frame.ba - frame.bb : null

  return (
    <Panel
      title="order book depth"
      right={
        <span className="text-[11px] text-zinc-500">
          spread {spread == null ? '--' : fmtInt(spread)}
        </span>
      }
    >
      <div className="flex h-full flex-col justify-center gap-3">
        <div className="flex flex-col-reverse gap-px">
          {asks.map((l) => (
            <Row key={`a-${l.price}`} level={l} maxCum={maxCum} side="ask" />
          ))}
        </div>

        <div className="flex items-center justify-center gap-2 py-1 text-[11px] text-zinc-500">
          <span className="h-px flex-1 bg-zinc-800" />
          <span>mid {frame.bb != null && frame.ba != null ? fmtInt((frame.bb + frame.ba) / 2) : '--'}</span>
          <span className="h-px flex-1 bg-zinc-800" />
        </div>

        <div className="flex flex-col gap-px">
          {bids.map((l) => (
            <Row key={`b-${l.price}`} level={l} maxCum={maxCum} side="bid" />
          ))}
        </div>
      </div>
    </Panel>
  )
}

interface CumLevel {
  price: number
  vol: number
  cum: number
}

// turn [price, volume] pairs (already inside-out) into a running cumulative total.
function cumulate(levels: [number, number][]): CumLevel[] {
  let running = 0
  return levels.map(([price, vol]) => {
    running += vol
    return { price, vol, cum: running }
  })
}

function Row({ level, maxCum, side }: { level: CumLevel; maxCum: number; side: 'bid' | 'ask' }) {
  const pct = Math.max(2, (level.cum / maxCum) * 100)
  const isBid = side === 'bid'
  return (
    <div className="relative flex h-6 items-center overflow-hidden rounded-sm bg-zinc-900/40">
      <div
        className={`absolute inset-y-0 ${isBid ? 'left-0 bg-bid/25' : 'left-0 bg-ask/25'}`}
        style={{ width: `${pct}%` }}
      />
      <div className="relative z-10 flex w-full items-center justify-between px-2 text-xs tabular-nums">
        <span className={isBid ? 'text-bid' : 'text-ask'}>{fmtInt(level.price)}</span>
        <span className="text-zinc-400">
          {fmtInt(level.vol)} <span className="text-zinc-600">/ {fmtInt(level.cum)}</span>
        </span>
      </div>
    </div>
  )
}

function Empty() {
  return <div className="flex h-full items-center justify-center text-sm text-zinc-600">no book data</div>
}
