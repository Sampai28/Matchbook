import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The engine is on 8080. Proxying rather than enabling CORS on the C++ server
// keeps the server free of browser-specific concerns, and means dev, preview
// and the container build all hit identical same-origin paths.
//
// Shared between `server` and `preview`: they are separate config blocks in
// Vite and `server.proxy` does NOT apply to `vite preview`. Without the second
// block, a preview build silently serves index.html for /stream — a 200 with
// an HTML body, which EventSource reports as a generic connection error.
const engineProxy = {
  '/order': 'http://localhost:8080',
  '/book': 'http://localhost:8080',
  '/snapshot': 'http://localhost:8080',
  '/stats': 'http://localhost:8080',
  '/healthz': 'http://localhost:8080',
  '/stream': {
    target: 'http://localhost:8080',
    changeOrigin: true,
    // Without this Vite buffers the proxied response and the SSE stream never
    // reaches the browser — the connection opens and then nothing arrives,
    // which looks exactly like a server bug.
    configure: (proxy: { on: (event: string, cb: (res: { headers: Record<string, string> }) => void) => void }) => {
      proxy.on('proxyRes', (proxyRes) => {
        proxyRes.headers['cache-control'] = 'no-cache';
        proxyRes.headers['x-accel-buffering'] = 'no';
      });
    },
  },
};

export default defineConfig({
  plugins: [react()],
  server: {
    host: true,
    port: 5173,
    proxy: engineProxy,
  },
  preview: {
    host: true,
    port: 4173,
    proxy: engineProxy,
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts'],
  },
});
