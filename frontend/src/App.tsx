import { useMemo, useState } from 'react'
import { derivePrints, type Frame } from './types'
import { usePlayback } from './hooks/usePlayback'
import { DepthChart } from './components/DepthChart'
import { PnlChart, type PnlPoint } from './components/PnlChart'
import { TickerTape } from './components/TickerTape'
import { PlaybackController } from './components/PlaybackController'
import { FileLoader } from './components/FileLoader'
import { fmtInt, fmtPnl } from './format'

export default function App() {
  const [frames, setFrames] = useState<Frame[]>([])
  const [fileName, setFileName] = useState<string>('')
  const pb = usePlayback(frames.length)

  const load = (f: Frame[], name: string) => {
    setFrames(f)
    setFileName(name)
    pb.pause()
    pb.setIndex(0)
  }

  const prints = useMemo(() => derivePrints(frames), [frames])
  const pnlSeries = useMemo<PnlPoint[]>(
    () => frames.map((f) => ({ t: f.t, pnl: f.pnl, pos: f.pos })),
    [frames],
  )

  const frame = frames[pb.index]
  const loaded = frames.length > 0

  return (
    <div className="flex min-h-full flex-col gap-4 p-4">
      <header className="flex flex-wrap items-center justify-between gap-3">
        <div className="flex items-baseline gap-3">
          <h1 className="text-lg font-bold tracking-tight text-zinc-100">
            chronos<span className="text-sky-400">·</span>bt
          </h1>
          <span className="text-xs uppercase tracking-widest text-zinc-500">replay dashboard</span>
          {fileName && <span className="text-xs text-zinc-600">{fileName}</span>}
        </div>
        <div className="flex items-center gap-4">
          {loaded && frame && <LiveStats pnl={frame.pnl} pos={frame.pos} />}
          <FileLoader onFrames={load} />
        </div>
      </header>

      {!loaded ? (
        <EmptyState />
      ) : (
        <>
          <main className="grid flex-1 grid-cols-1 gap-4 lg:grid-cols-2">
            <DepthChart frame={frame} />
            <PnlChart series={pnlSeries} index={pb.index} />
            <div className="lg:col-span-2">
              <TickerTape prints={prints} index={pb.index} />
            </div>
          </main>
          <PlaybackController pb={pb} frames={frames} />
        </>
      )}
    </div>
  )
}

function LiveStats({ pnl, pos }: { pnl: number; pos: number }) {
  return (
    <div className="flex items-center gap-4 text-sm tabular-nums">
      <div className="flex flex-col items-end">
        <span className="text-[10px] uppercase tracking-wider text-zinc-500">pnl</span>
        <span className={pnl >= 0 ? 'text-bid' : 'text-ask'}>{fmtPnl(pnl)}</span>
      </div>
      <div className="flex flex-col items-end">
        <span className="text-[10px] uppercase tracking-wider text-zinc-500">pos</span>
        <span className={pos >= 0 ? 'text-bid' : 'text-ask'}>
          {pos > 0 ? `+${fmtInt(pos)}` : fmtInt(pos)}
        </span>
      </div>
    </div>
  )
}

function EmptyState() {
  return (
    <div className="flex flex-1 flex-col items-center justify-center gap-4 rounded-lg border border-dashed border-zinc-800 bg-panel/40 text-center">
      <div className="text-5xl">📈</div>
      <div className="max-w-md space-y-2">
        <h2 className="text-base font-semibold text-zinc-200">load a simulation trace</h2>
        <p className="text-sm leading-relaxed text-zinc-500">
          upload a <code className="rounded bg-zinc-800 px-1 text-zinc-300">.jsonl</code> trace
          produced by the chronos-bt logger, or load the bundled sample. the dashboard replays the
          order book depth, realized p&l curve &amp; execution prints frame by frame.
        </p>
        <p className="text-xs text-zinc-600">
          generate one with <code className="rounded bg-zinc-800 px-1 text-zinc-300">./build/micro_price_mm</code>{' '}
          &rarr; <code className="rounded bg-zinc-800 px-1 text-zinc-300">trace.jsonl</code>
        </p>
      </div>
    </div>
  )
}
