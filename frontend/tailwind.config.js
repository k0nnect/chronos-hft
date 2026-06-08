/** @type {import('tailwindcss').Config} */
export default {
  // dark mode is driven by the `dark` class on <html> (set in index.html); the
  // palette below is already dark by default so the dashboard never flashes light.
  darkMode: 'class',
  content: ['./index.html', './src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        // book sides: teal bid, red ask -- the standard exchange convention.
        bid: '#26a69a',
        ask: '#ef5350',
        pnl: '#38bdf8',
        panel: '#0c0f14',
      },
      fontFamily: {
        mono: ['ui-monospace', 'SFMono-Regular', 'Menlo', 'Consolas', 'monospace'],
      },
    },
  },
  plugins: [],
}
