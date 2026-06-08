import type { Playback } from '../hooks/usePlayback'
import type { Frame } from '../types'
import { fmtInt, fmtTime } from '../format'

const SPEEDS = [0.5, 1, 2, 4, 8]

// transport bar: play/pause, step, speed multipliers & a scrubber that seeks to
// any frame in the loaded trace.
export function PlaybackController({
  pb,
  frames,
}: {
  pb: Playback
  frames: Frame[]
}) {
  const count = frames.length
  const atEnd = pb.index >= count - 1
  const frame = frames[pb.index]

  return (
    <div className="flex flex-col gap-2 rounded-lg border border-zinc-800 bg-panel/80 px-4 py-3 shadow-lg">
      <div className="flex flex-wrap items-center gap-3">
        <button
          onClick={() => (atEnd ? (pb.setIndex(0), pb.play()) : pb.toggle())}
          className="flex h-9 w-20 items-center justify-center gap-2 rounded-md bg-sky-500/90 text-sm font-semibold text-zinc-950 transition-colors hover:bg-sky-400"
        >
          {pb.playing ? 'pause' : atEnd ? 'replay' : 'play'}
        </button>

        <div className="flex items-center gap-1">
          <StepButton label="« -10" onClick={() => pb.stepBy(-10)} />
          <StepButton label="‹ -1" onClick={() => pb.stepBy(-1)} />
          <StepButton label="+1 ›" onClick={() => pb.stepBy(1)} />
          <StepButton label="+10 »" onClick={() => pb.stepBy(10)} />
        </div>

        <div className="flex items-center gap-1">
          {SPEEDS.map((s) => (
            <button
              key={s}
              onClick={() => pb.setSpeed(s)}
              className={`h-8 rounded-md px-2.5 text-xs font-semibold transition-colors ${
                pb.speed === s
                  ? 'bg-zinc-200 text-zinc-950'
                  : 'bg-zinc-800/70 text-zinc-400 hover:bg-zinc-700'
              }`}
            >
              {s}x
            </button>
          ))}
        </div>

        <div className="ml-auto flex items-center gap-4 text-xs tabular-nums text-zinc-400">
          <span>{frame ? fmtTime(frame.t) : 't=--'}</span>
          <span className="text-zinc-600">
            frame {count === 0 ? 0 : pb.index + 1} / {count}
          </span>
        </div>
      </div>

      <input
        type="range"
        min={0}
        max={Math.max(0, count - 1)}
        value={pb.index}
        onChange={(e) => pb.setIndex(Number(e.target.value))}
        className="h-1.5 w-full cursor-pointer appearance-none rounded-full bg-zinc-800 accent-sky-500"
      />

      {frame && (
        <div className="flex flex-wrap items-center gap-x-6 gap-y-1 text-xs tabular-nums text-zinc-500">
          <span>
            bid <span className="text-bid">{frame.bb == null ? '--' : fmtInt(frame.bb)}</span>
          </span>
          <span>
            ask <span className="text-ask">{frame.ba == null ? '--' : fmtInt(frame.ba)}</span>
          </span>
          <span>
            pos{' '}
            <span className={frame.pos >= 0 ? 'text-bid' : 'text-ask'}>
              {frame.pos > 0 ? `+${fmtInt(frame.pos)}` : fmtInt(frame.pos)}
            </span>
          </span>
        </div>
      )}
    </div>
  )
}

function StepButton({ label, onClick }: { label: string; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className="h-8 rounded-md bg-zinc-800/70 px-2 text-xs text-zinc-300 transition-colors hover:bg-zinc-700"
    >
      {label}
    </button>
  )
}
