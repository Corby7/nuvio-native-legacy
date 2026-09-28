// Builds deploy/app/art/tv-logos.txt, the index Live TV looks channel logos up
// in (src/tvlogos.c), from the community tv-logo/tv-logos repository.
//
// Why: IPTV providers ship 96px logos, often a grey variant, and the player's
// loading logo and the tiles blow them up into a blur. tv-logos has most
// broadcast channels at 512px on a transparent ground.
//
// The index pins the repo's COMMIT, so every URL in it stays valid however the
// repo moves on; rerun this to pick up new logos.
//
//   node tools/build-tv-logos.mjs
//
// Each line is "slug<TAB>suffix<TAB>path": the slug is the channel's name as
// tvlogos.c normalises a playlist name (quality tags dropped), the suffix the
// file's country or region code, the path relative to countries/ without .png.
// Sorted by slug, then suffix, for a binary search on the device.
import fs from 'node:fs';
import path from 'node:path';

const REPO = 'tv-logo/tv-logos';
const headers = { 'User-Agent': 'nuvio-build', Accept: 'application/vnd.github+json' };
const get = async (url) => {
  const r = await fetch(url, { headers, signal: AbortSignal.timeout(60000) });
  if (!r.ok) throw Error(`${url}: ${r.status}`);
  return r.json();
};

const commit = (await get(`https://api.github.com/repos/${REPO}/commits/main`)).sha;
const tree = await get(`https://api.github.com/repos/${REPO}/git/trees/${commit}?recursive=1`);
if (tree.truncated) throw Error('tree truncated: the index would be partial');

// Must match STOP in src/tvlogos.c: tokens that say how a feed is carried, not
// which channel it is.
const STOP = new Set(['hd', 'fhd', 'uhd', 'sd', '4k', '8k', 'raw', 'hevc', 'h265', 'h264',
  '50fps', '60fps', 'vip', 'lq', 'hq', 'eon', 'backup']);

const best = new Map();   // "slug\tsuffix" -> { path, rank }
for (const e of tree.tree) {
  const p = e.path;
  if (e.type !== 'blob' || !p.startsWith('countries/') || !p.endsWith('.png')) continue;
  const rel = p.slice('countries/'.length, -'.png'.length);
  if (rel.includes('/old/')) continue;
  const m = /^(.*)-([a-z]{2,5})$/.exec(rel.split('/').pop());
  if (!m) continue;
  const tokens = m[1].split('-');
  const slug = tokens.filter((t) => t && !STOP.has(t)).join('-');
  if (!slug) continue;
  // The plain file over its -hd twin, a folder's own file over a sub-folder's.
  const rank = (tokens.length !== slug.split('-').length ? 2 : 0) + (rel.split('/').length > 2 ? 1 : 0);
  const key = `${slug}\t${m[2]}`;
  const was = best.get(key);
  if (!was || rank < was.rank) best.set(key, { path: rel, rank });
}

const lines = [...best.entries()].map(([k, v]) => `${k}\t${v.path}`)
  .sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
const out = path.resolve(import.meta.dirname, '../deploy/app/art/tv-logos.txt');
fs.writeFileSync(out, `@${commit}\n${lines.join('\n')}\n`);
console.log(`${lines.length} logos from ${REPO}@${commit.slice(0, 10)} -> ${path.relative(process.cwd(), out)}`);
