import { useEffect, useState } from 'react'

export interface Playback {
  index: number
  playing: boolean
  speed: number
  setIndex: (i: number) => void
  setSpeed: (s: number) => void
  play: () => void
  pause: () => void
  toggle: () => void
  stepBy: (d: number) => void
}

// drives a frame cursor through [0, count). one frame is ~100 ms of sim time, so
// 1x advances ten frames a second; `speed` multiplies that rate. the timer is the
// single source of advancement -- the scrubber & step buttons just set the index.
export function usePlayback(count: number): Playback {
  const [index, setIndexRaw] = useState(0)
  const [playing, setPlaying] = useState(false)
  const [speed, setSpeed] = useState(1)

  const clamp = (i: number): number => {
    if (count <= 0) return 0
    return Math.max(0, Math.min(count - 1, i))
  }
  const setIndex = (i: number): void => setIndexRaw(clamp(i))
  const stepBy = (d: number): void => setIndexRaw((i) => clamp(i + d))

  // keep the cursor in range when a new (shorter/longer) file is loaded.
  useEffect(() => {
    setIndexRaw((i) => clamp(i))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [count])

  // the playback timer: tick the index forward, stopping at the final frame.
  useEffect(() => {
    if (!playing || count === 0) return
    const periodMs = 100 / speed
    const id = window.setInterval(() => {
      setIndexRaw((i) => {
        if (i >= count - 1) return i
        return i + 1
      })
    }, periodMs)
    return () => window.clearInterval(id)
  }, [playing, speed, count])

  // auto-pause once the end is reached.
  useEffect(() => {
    if (playing && count > 0 && index >= count - 1) setPlaying(false)
  }, [index, playing, count])

  return {
    index,
    playing,
    speed,
    setIndex,
    setSpeed,
    play: () => setPlaying(true),
    pause: () => setPlaying(false),
    toggle: () => setPlaying((p) => !p),
    stepBy,
  }
}
