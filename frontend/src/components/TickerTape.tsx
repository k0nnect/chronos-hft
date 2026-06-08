import { Panel } from './Panel'
import type { Print } from '../types'
import { fmtInt } from '../format'

// a scrolling tape of the most recent execution prints, newest on the left. only
// prints at or before the current frame are shown, so the tape stays in sync with
// the depth & p&l panels as playback advances.
export function TickerTape({ prints, index }: { prints: Print[]; index: number }) {
  const visible: Print[] = []
  for (let i = prints.length - 1; i >= 0 && visible.length < 60; i--) {
    if (prints[i].idx <= index) visible.push(prints[i])
  }

  return (
    <Panel title="execution prints">
      <div className="flex h-full items-center overflow-x-auto">
        {visible.length === 0 ? (
          <span className="px-2 text-sm text-zinc-600">no prints yet</span>
        ) : (
          <div className="flex items-center gap-2">
            {visible.map((p, i) => (
              <Tag key={`${p.idx}-${i}`} print={p} fresh={i === 0} />
            ))}
          </div>
        )}
      </div>
    </Panel>
  )
}

function Tag({ print, fresh }: { print: Print; fresh: boolean }) {
  const buy = print.side === 'buy'
  return (
    <div
      className={`flex shrink-0 items-center gap-2 rounded-md border px-2.5 py-1.5 text-xs tabular-nums transition-colors ${
        buy ? 'border-bid/40 bg-bid/10' : 'border-ask/40 bg-ask/10'
      } ${fresh ? 'ring-1 ring-inset ' + (buy ? 'ring-bid/60' : 'ring-ask/60') : ''}`}
    >
      <span className={`font-bold ${buy ? 'text-bid' : 'text-ask'}`}>{buy ? '▲' : '▼'}</span>
      <span className={buy ? 'text-bid' : 'text-ask'}>{fmtInt(print.qty)}</span>
      <span className="text-zinc-500">@</span>
      <span className="text-zinc-200">{fmtInt(print.price)}</span>
      <span className="text-zinc-600">·{fmtInt(print.t)}</span>
    </div>
  )
}
