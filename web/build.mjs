#!/usr/bin/env node
// Builds firmware/src/generated/web_assets.h from the web/ assets.
// Node built-ins only (node:fs, node:zlib, node:path, node:crypto). No npm deps.
//
// Usage:
//   node web/build.mjs           # (re)generate the header
//   node web/build.mjs --check   # exit non-zero if the committed header is stale (for CI)

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const webDir = __dirname;
const outPath = path.join(__dirname, '..', 'firmware', 'src', 'generated', 'web_assets.h');

const ASSETS = [
  { file: 'index.html', name: 'INDEX_HTML', urlPath: '/', contentType: 'text/html; charset=utf-8' },
  { file: 'app.js', name: 'APP_JS', urlPath: '/app.js', contentType: 'application/javascript; charset=utf-8' },
  { file: 'app.css', name: 'APP_CSS', urlPath: '/app.css', contentType: 'text/css; charset=utf-8' },
  { file: 'favicon.svg', name: 'FAVICON_SVG', urlPath: '/favicon.svg', contentType: 'image/svg+xml' },
];

function gzipBytes(buf) {
  return zlib.gzipSync(buf, { level: 9 });
}

/* ---- Comment stripping for the embedded copy only.
   app.js and app.css are heavily commented on purpose — the reasoning behind a piece of
   UI is worth more in the file than in a commit message — but the ESP32 pays for every
   byte of it in flash. This drops whole-line comments from the copy that gets gzipped
   into the firmware; web/ on disk, the mock server, and anything a developer reads are
   untouched.

   Deliberately conservative: only lines that are *entirely* a comment go. A trailing
   comment after code stays, because deciding where the code ends needs a real tokenizer.
   The one way a whole-line rule can corrupt a file is a multi-line template literal (or
   a multi-line string) containing a line that starts with `//`, so assertNoMultilineStrings
   below refuses to run at all if the source ever grows one, and the result is parsed
   before it is used. */

// A template literal spanning lines leaves an odd number of unescaped backticks on the
// line that opens it. Same idea for ' and " (which cannot span lines without a
// continuation anyway). Cheap, and it fails loudly rather than silently mangling.
function assertNoMultilineStrings(src, file) {
  const lines = src.split('\n');
  for (let i = 0; i < lines.length; i++) {
    const ticks = (lines[i].replace(/\\./g, '').match(/`/g) || []).length;
    if (ticks % 2 === 1) {
      throw new Error(
        `${file}:${i + 1}: a template literal spans lines, so build.mjs cannot safely strip `
        + 'comments. Put it on one line, or teach stripComments() to tokenize.');
    }
  }
}

// Removes whole-line // comments and whole-line /* */ blocks. Blank lines left behind by
// a removed comment go too; blank lines that were already there are kept, so the shape of
// the file survives for anyone reading the served copy.
function stripComments(src, file) {
  assertNoMultilineStrings(src, file);
  const out = [];
  let inBlock = false;
  for (const line of src.split('\n')) {
    const t = line.trim();
    if (inBlock) {
      if (t.endsWith('*/')) inBlock = false;
      continue;
    }
    if (t.startsWith('/*')) {
      // A block that opens and closes on this line is only droppable if nothing follows it.
      const end = t.indexOf('*/');
      if (end === -1) { inBlock = true; continue; }
      if (t.slice(end + 2).trim() === '') continue;
      out.push(line);
      continue;
    }
    if (t.startsWith('//')) continue;
    out.push(line);
  }
  return out.join('\n');
}

// Parse-check without executing: new Function compiles the body and throws on a syntax
// error, so a stripping bug cannot reach the device as a broken script.
function assertParses(src, file) {
  try {
    new Function(src); // eslint-disable-line no-new-func
  } catch (e) {
    throw new Error(`${file}: the comment-stripped copy does not parse (${e.message}). This is a build bug.`);
  }
}

function minifyForFlash(buf, file) {
  if (file.endsWith('.js')) {
    const stripped = stripComments(buf.toString('utf8'), file);
    assertParses(stripped, file);
    return Buffer.from(stripped, 'utf8');
  }
  if (file.endsWith('.css')) {
    // CSS has no template literals; the same whole-line rule applies to /* */ blocks.
    return Buffer.from(stripComments(buf.toString('utf8'), file), 'utf8');
  }
  return buf;
}

function toCArray(buf) {
  const parts = [];
  for (let i = 0; i < buf.length; i++) parts.push('0x' + buf[i].toString(16).padStart(2, '0'));
  // Wrap lines to keep the generated header readable and diff-friendly.
  const lines = [];
  for (let i = 0; i < parts.length; i += 20) lines.push('  ' + parts.slice(i, i + 20).join(', ') + (i + 20 < parts.length ? ',' : ''));
  return lines.join('\n');
}

function buildHeader() {
  const compiled = ASSETS.map((a) => {
    const source = fs.readFileSync(path.join(webDir, a.file));
    // `raw` is what gets embedded (comments stripped for JS/CSS); `source` is the file on
    // disk, reported alongside it so the saving is visible on every build.
    const raw = minifyForFlash(source, a.file);
    const gz = gzipBytes(raw);
    return { ...a, source, raw, gz };
  });

  const hashInput = compiled.map((a) => a.raw).reduce((acc, b) => Buffer.concat([acc, b]), Buffer.alloc(0));
  const etag = crypto.createHash('sha256').update(hashInput).digest('hex').slice(0, 16);

  // Cache busting: index.html is served with Cache-Control: no-cache (revalidated on every
  // navigation), but a browser's normal reload keeps fresh subresources, so a firmware update
  // could show the old app.js/app.css for up to max-age (an hour). Referencing them with the
  // content hash in the query string makes every new build fetch new files; the device ignores
  // the query when matching /app.js and /app.css.
  for (const a of compiled) {
    if (a.file !== 'index.html') continue;
    const html = a.raw.toString('utf8').replace('href="/app.css"', `href="/app.css?v=${etag}"`).replace('src="/app.js"', `src="/app.js?v=${etag}"`);
    if (!html.includes(`app.js?v=${etag}`) || !html.includes(`app.css?v=${etag}`)) throw new Error('index.html: expected href="/app.css" and src="/app.js" to version');
    a.raw = Buffer.from(html, 'utf8');
    a.gz = gzipBytes(a.raw);
  }

  const totalRaw = compiled.reduce((n, a) => n + a.raw.length, 0);
  const totalGz = compiled.reduce((n, a) => n + a.gz.length, 0);
  const totalSource = compiled.reduce((n, a) => n + a.source.length, 0);

  const lines = [];
  lines.push('// GENERATED FILE — do not edit by hand.');
  lines.push('// Produced by web/build.mjs from web/index.html, web/app.js, web/app.css, web/favicon.svg.');
  lines.push('// Run `node web/build.mjs` to regenerate; `node web/build.mjs --check` verifies freshness (used by CI).');
  lines.push('//');
  lines.push('// JS/CSS are embedded with whole-line comments stripped (see stripComments in build.mjs);');
  lines.push('// web/ on disk keeps them. "Embedded sizes" below are after that strip.');
  lines.push(`// On-disk sizes:  ${compiled.map((a) => `${a.file}=${a.source.length}B`).join(', ')} (total ${totalSource}B)`);
  lines.push(`// Embedded sizes: ${compiled.map((a) => `${a.file}=${a.raw.length}B`).join(', ')} (total ${totalRaw}B)`);
  lines.push(`// Gzip sizes:     ${compiled.map((a) => `${a.file}=${a.gz.length}B`).join(', ')} (total ${totalGz}B)`);
  lines.push('#pragma once');
  lines.push('#include <stddef.h>');
  lines.push('#include <stdint.h>');
  lines.push('');
  lines.push('// Arduino headers define PROGMEM; on the host build (native tests, this script) it is a no-op');
  lines.push('// so the same header compiles both on ESP32 and on a desktop toolchain.');
  lines.push('#ifndef PROGMEM');
  lines.push('#define PROGMEM');
  lines.push('#endif');
  lines.push('');
  lines.push('namespace transit_web {');
  lines.push('');

  for (const a of compiled) {
    lines.push(`// ${a.file} — ${a.raw.length} bytes raw, ${a.gz.length} bytes gzip`);
    lines.push(`static const uint8_t ${a.name}_GZ[] PROGMEM = {`);
    lines.push(toCArray(a.gz));
    lines.push('};');
    lines.push(`static const size_t ${a.name}_GZ_LEN = ${a.gz.length};`);
    lines.push('');
  }

  lines.push('struct WebAsset {');
  lines.push('  const char* path;');
  lines.push('  const char* content_type;');
  lines.push('  const uint8_t* data;');
  lines.push('  size_t len;');
  lines.push('};');
  lines.push('');
  lines.push('static const WebAsset WEB_ASSETS[] = {');
  for (const a of compiled) {
    lines.push(`  { "${a.urlPath}", "${a.contentType}", ${a.name}_GZ, ${a.name}_GZ_LEN },`);
  }
  lines.push('};');
  lines.push('static const size_t WEB_ASSETS_COUNT = sizeof(WEB_ASSETS) / sizeof(WEB_ASSETS[0]);');
  lines.push('');
  lines.push(`static const char WEB_ASSETS_ETAG[] = "${etag}";`);
  lines.push('');
  lines.push('}  // namespace transit_web');
  lines.push('');

  return { text: lines.join('\n'), compiled, totalSource, totalRaw, totalGz };
}

function report(compiled, totalSource, totalRaw, totalGz) {
  for (const a of compiled) {
    const stripped = a.source.length - a.raw.length;
    const note = stripped > 0 ? ` (${stripped}B of comments stripped)` : '';
    console.log(`  ${a.file}: ${a.source.length}B on disk -> ${a.raw.length}B embedded${note} -> ${a.gz.length}B gzip`);
  }
  console.log(`  total: ${totalSource}B on disk -> ${totalRaw}B embedded -> ${totalGz}B gzip`);
}

function main() {
  const checkMode = process.argv.includes('--check');
  const { text, compiled, totalSource, totalRaw, totalGz } = buildHeader();

  if (checkMode) {
    if (!fs.existsSync(outPath)) {
      console.error(`web/build.mjs --check: ${outPath} does not exist. Run "node web/build.mjs" first.`);
      process.exit(1);
    }
    const existing = fs.readFileSync(outPath, 'utf8');
    if (existing !== text) {
      console.error('web/build.mjs --check: firmware/src/generated/web_assets.h is stale. Run "node web/build.mjs" and commit the result.');
      process.exit(1);
    }
    console.log('web/build.mjs --check: web_assets.h is up to date.');
    report(compiled, totalSource, totalRaw, totalGz);
    return;
  }

  fs.mkdirSync(path.dirname(outPath), { recursive: true });
  fs.writeFileSync(outPath, text);
  console.log(`Wrote ${outPath}`);
  report(compiled, totalSource, totalRaw, totalGz);
  console.log('  (budget: 60000B gzip, DESIGN.md §10)');
  if (totalGz > 60000) {
    console.error(`WARNING: total gzip size ${totalGz}B exceeds the 60KB budget in DESIGN.md §10.`);
  }
}

main();
