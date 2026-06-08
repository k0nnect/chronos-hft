import { useRef, useState } from 'react'
import { parseJsonl, type Frame } from '../types'

// loads a .jsonl trace either from a user-picked file or the bundled sample, then
// hands the parsed frames up to the app. all parsing is client-side; nothing is
// uploaded anywhere.
export function FileLoader({ onFrames }: { onFrames: (frames: Frame[], name: string) => void }) {
  const inputRef = useRef<HTMLInputElement>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)

  const ingest = async (text: string, name: string) => {
    const frames = parseJsonl(text)
    if (frames.length === 0) {
      setError('no valid frames in that file')
      return
    }
    setError(null)
    onFrames(frames, name)
  }

  const onPick = async (file: File | undefined) => {
    if (!file) return
    setBusy(true)
    try {
      const text = await file.text()
      await ingest(text, file.name)
    } catch {
      setError('could not read that file')
    } finally {
      setBusy(false)
    }
  }

  const loadSample = async () => {
    setBusy(true)
    try {
      const res = await fetch('sample-trace.jsonl')
      if (!res.ok) throw new Error('missing')
      await ingest(await res.text(), 'sample-trace.jsonl')
    } catch {
      setError('run ./build/micro_price_mm, then copy trace.jsonl into frontend/public/')
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="flex items-center gap-2">
      <input
        ref={inputRef}
        type="file"
        accept=".jsonl,.json,.txt,application/json"
        className="hidden"
        onChange={(e) => onPick(e.target.files?.[0] ?? undefined)}
      />
      <button
        onClick={() => inputRef.current?.click()}
        disabled={busy}
        className="h-9 rounded-md bg-zinc-200 px-3 text-sm font-semibold text-zinc-950 transition-colors hover:bg-white disabled:opacity-50"
      >
        upload jsonl
      </button>
      <button
        onClick={loadSample}
        disabled={busy}
        className="h-9 rounded-md border border-zinc-700 px-3 text-sm text-zinc-300 transition-colors hover:bg-zinc-800 disabled:opacity-50"
      >
        load sample
      </button>
      {error && <span className="text-xs text-ask">{error}</span>}
    </div>
  )
}
