// Pre-compresses text assets next to their source, as `<file>.gz`.
//
// CMake has no portable raw-gzip primitive (file(ARCHIVE_CREATE) is tar-based),
// so this runs in the Node stage that already exists to build the app. The C++
// side embeds BOTH variants and picks by Accept-Encoding.
//
// Only compressible types are gzipped: a .woff2 or .png is already compressed,
// and embedding a second, larger copy would waste .rodata for nothing.

import { createReadStream, createWriteStream } from 'node:fs';
import { readdir, stat, unlink } from 'node:fs/promises';
import { pipeline } from 'node:stream/promises';
import { createGzip } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { join, extname } from 'node:path';

const DIST = fileURLToPath(new URL('../dist/', import.meta.url));
const COMPRESSIBLE = new Set(['.html', '.js', '.css', '.svg', '.json', '.map']);

async function* walk(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const path = join(dir, entry.name);
    if (entry.isDirectory()) {
      yield* walk(path);
    } else {
      yield path;
    }
  }
}

let compressed = 0;
for await (const path of walk(DIST)) {
  if (!COMPRESSIBLE.has(extname(path))) continue;

  const target = `${path}.gz`;
  await pipeline(
    createReadStream(path),
    // Level 9: this runs once at build time and the output ships in a binary,
    // so build seconds are worth bytes.
    createGzip({ level: 9 }),
    createWriteStream(target),
  );

  // A .gz that is not smaller is dead weight in .rodata. Keeping it would be
  // strictly worse than serving the raw bytes.
  const [raw, gz] = await Promise.all([stat(path), stat(target)]);
  if (gz.size >= raw.size) {
    await unlink(target);
    continue;
  }
  compressed += 1;
}

console.log(`gzip-dist: compressed ${compressed} file(s)`);
