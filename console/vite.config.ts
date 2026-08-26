import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';

// The console is served from /console/ by the C++ engine, so every emitted URL
// must be relative to that base -- an absolute /assets/... would 404.
export default defineConfig({
  plugins: [preact()],
  base: '/console/',
  build: {
    // One JS chunk and one CSS file. Code-splitting would buy nothing here
    // (three pages, all reachable immediately) and each extra chunk is another
    // byte array in the binary and another entry in the asset table.
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        manualChunks: undefined,
      },
    },
    // Inlining would put asset bytes into index.html, which is the one file
    // served with no-cache -- so every reload would re-download them.
    assetsInlineLimit: 0,
    target: 'es2022',
    sourcemap: false,
  },
});
