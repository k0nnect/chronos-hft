// small display formatters shared across the panels.

export const fmtInt = (n: number): string => Math.round(n).toLocaleString('en-US')

export const fmtPnl = (n: number): string =>
  (n >= 0 ? '+' : '') +
  n.toLocaleString('en-US', { minimumFractionDigits: 2, maximumFractionDigits: 2 })

export const fmtPrice = (n: number | null): string =>
  n == null ? '--' : n.toLocaleString('en-US', { maximumFractionDigits: 2 })

export const fmtSigned = (n: number): string => (n > 0 ? `+${fmtInt(n)}` : fmtInt(n))

// the synthetic feed stamps message-index nanoseconds, so render the raw tick
// count with thousands separators rather than pretending it is wall-clock time.
export const fmtTime = (ns: number): string => `t=${fmtInt(ns)}`
