// the shape of one jsonl frame emitted by the c++ trace_logger, plus the helpers
// that parse a whole file & derive execution prints from it.

export interface Frame {
  t: number // simulation timestamp (nanoseconds for real captures)
  bb: number | null // best bid price in ticks (null when the bid side is empty)
  ba: number | null // best ask price in ticks
  bids: [number, number][] // top levels, inside-out: [price, volume]
  asks: [number, number][]
  pos: number // signed position in shares
  pnl: number // realized p&l
}

// a synthetic execution print, derived from a position change between two frames.
// the logger snapshots state rather than every fill, so a position delta is our
// proxy for "a trade happened" -- enough to drive the ticker tape.
export interface Print {
  idx: number // frame index the print is attributed to
  t: number
  side: 'buy' | 'sell'
  qty: number
  price: number
}

// parse a jsonl blob into frames, skipping blank or malformed lines so a
// partially-written trace still loads.
export function parseJsonl(text: string): Frame[] {
  const frames: Frame[] = []
  for (const raw of text.split('\n')) {
    const line = raw.trim()
    if (line.length === 0) continue
    try {
      const obj = JSON.parse(line) as Frame
      if (typeof obj.t === 'number' && Array.isArray(obj.bids) && Array.isArray(obj.asks)) {
        frames.push(obj)
      }
    } catch {
      // a malformed / truncated trailing line is skipped, not fatal.
    }
  }
  return frames
}

// arithmetic mid from a frame, or whichever side exists, or null on an empty book.
export function midOf(f: Frame): number | null {
  if (f.bb != null && f.ba != null) return (f.bb + f.ba) / 2
  if (f.bb != null) return f.bb
  if (f.ba != null) return f.ba
  return null
}

// build the execution-print timeline once per loaded file: every frame whose
// position differs from the previous one becomes a print, signed by the delta.
export function derivePrints(frames: Frame[]): Print[] {
  const out: Print[] = []
  for (let i = 1; i < frames.length; i++) {
    const delta = frames[i].pos - frames[i - 1].pos
    if (delta !== 0) {
      const mid = midOf(frames[i])
      out.push({
        idx: i,
        t: frames[i].t,
        side: delta > 0 ? 'buy' : 'sell',
        qty: Math.abs(delta),
        price: mid ?? 0,
      })
    }
  }
  return out
}
