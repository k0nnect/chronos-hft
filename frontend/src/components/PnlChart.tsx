import {
  Area,
  AreaChart,
  CartesianGrid,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { Panel } from './Panel'
import { fmtInt, fmtPnl } from '../format'

export interface PnlPoint {
  t: number
  pnl: number
  pos: number
}

// realized-p&l curve revealed up to the current playback frame. an area fill +
// a zero reference line make gains/losses readable at a glance.
export function PnlChart({ series, index }: { series: PnlPoint[]; index: number }) {
  const data = series.slice(0, Math.max(1, index + 1))
  const last = data[data.length - 1]
  const pnl = last ? last.pnl : 0

  return (
    <Panel
      title="realized p&l"
      right={
        <span className={`text-sm font-semibold tabular-nums ${pnl >= 0 ? 'text-bid' : 'text-ask'}`}>
          {fmtPnl(pnl)}
        </span>
      }
    >
      <div className="h-full min-h-[180px] w-full">
        <ResponsiveContainer width="100%" height="100%">
          <AreaChart data={data} margin={{ top: 8, right: 12, bottom: 4, left: 4 }}>
            <defs>
              <linearGradient id="pnlFill" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor="#38bdf8" stopOpacity={0.45} />
                <stop offset="100%" stopColor="#38bdf8" stopOpacity={0.02} />
              </linearGradient>
            </defs>
            <CartesianGrid stroke="#1f2937" strokeDasharray="3 3" vertical={false} />
            <XAxis
              dataKey="t"
              tick={{ fill: '#52525b', fontSize: 10 }}
              tickFormatter={(v) => fmtInt(Number(v))}
              stroke="#27272a"
              minTickGap={48}
            />
            <YAxis
              tick={{ fill: '#52525b', fontSize: 10 }}
              tickFormatter={(v) => fmtInt(Number(v))}
              stroke="#27272a"
              width={56}
            />
            <Tooltip
              contentStyle={{
                background: '#0c0f14',
                border: '1px solid #27272a',
                borderRadius: 8,
                fontSize: 12,
              }}
              labelStyle={{ color: '#a1a1aa' }}
              labelFormatter={(label) => `t=${fmtInt(Number(label))}`}
              formatter={(value) => [fmtPnl(Number(value)), 'pnl']}
            />
            <ReferenceLine y={0} stroke="#3f3f46" strokeWidth={1} />
            <Area
              type="monotone"
              dataKey="pnl"
              stroke="#38bdf8"
              strokeWidth={1.75}
              fill="url(#pnlFill)"
              isAnimationActive={false}
              dot={false}
            />
          </AreaChart>
        </ResponsiveContainer>
      </div>
    </Panel>
  )
}
