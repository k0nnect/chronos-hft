import type { ReactNode } from 'react'

// shared panel chrome: a titled, bordered dark card the charts sit inside.
export function Panel({
  title,
  right,
  children,
  className = '',
}: {
  title: string
  right?: ReactNode
  children: ReactNode
  className?: string
}) {
  return (
    <section
      className={`flex flex-col rounded-lg border border-zinc-800 bg-panel/80 shadow-lg ${className}`}
    >
      <header className="flex items-center justify-between border-b border-zinc-800 px-4 py-2">
        <h2 className="text-xs font-semibold uppercase tracking-widest text-zinc-400">{title}</h2>
        {right}
      </header>
      <div className="min-h-0 flex-1 p-3">{children}</div>
    </section>
  )
}
