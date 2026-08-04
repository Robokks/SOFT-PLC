import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  // In dev, the frontend runs on Vite's own port while PlcServer runs separately
  // (see docs/architecture.md's "Web frontend" section) -- proxy /api so the app can
  // always call same-origin paths, matching how it's served in production once
  // PlcServer mounts the built dist/ directory.
  server: {
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
    },
  },
})
