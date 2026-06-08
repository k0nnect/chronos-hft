import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// minimal vite setup: react fast-refresh in dev, static bundle on build.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    open: false,
  },
})
