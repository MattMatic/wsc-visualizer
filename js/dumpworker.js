// Worker for comparing shaping dumps (*.dws.txt / *.cts.txt) with HarfBuzz. Several of these run in parallel (index.html: buildFromDumps).
// Classic worker (the emscripten HarfBuzz build is a classic script); shapedump-core.mjs is loaded with a dynamic import.
// Each worker keeps a cache of its HarfBuzz results (word + paragraph direction + itemisation mode), and words are always sent to the
// same worker (by hash), so loading a second dump of the same corpus, or the same dump again, does not reshape the words.
'use strict';
importScripts('hbjs-12.3.0/hb.js', 'hbjs.js');

let hb = null, core = null, blob = null, face = null, font = null;
let script = '', language = '', hmtx = null, blankCache = new Map(), cache = new Map();
const latin = /\p{Script=Latin}/u;
const ready = (async () => {
  hb = hbjs(await createHarfBuzz({ locateFile: (f) => new URL('hbjs-12.3.0/' + f, self.location.href).href }));
  core = await import('./shapedump-core.mjs?v=3');
})();

// nominal advances straight from hmtx (scaled to 1000 units)
function readHmtx(buf) {
  const dv = new DataView(buf), n = dv.getUint16(4), tabs = {};
  for (let i = 0; i < n; i++) { const o = 12 + 16 * i; tabs[String.fromCharCode(dv.getUint8(o), dv.getUint8(o + 1), dv.getUint8(o + 2), dv.getUint8(o + 3))] = dv.getUint32(o + 8); }
  const upem = dv.getUint16(tabs.head + 18), nh = dv.getUint16(tabs.hhea + 34), base = tabs.hmtx;
  return (g) => { const k = Math.min(g, nh - 1); return Math.round(dv.getUint16(base + 4 * k) * 1000 / upem); };
}

function shapeRun(txt, rtl, itemise) {
  const lat = itemise && latin.test(txt);
  const buffer = hb.createBuffer();
  buffer.addText(txt);
  buffer.guessSegmentProperties();
  buffer.setDirection(rtl ? 'rtl' : 'ltr');
  if (lat) { buffer.setScript('latn'); buffer.setLanguage('en'); }
  else { if (script) buffer.setScript(script); if (language) buffer.setLanguage(language); }
  hb.shape(font, buffer);
  const r = buffer.json(font).map((g) => [g.g, g.dx, g.dy, g.ax]);
  buffer.destroy();
  return r;
}
const shaper = {
  shape(word, rtl, engine) {
    const key = engine + (rtl ? '1' : '0') + word;
    let v = cache.get(key);
    if (!v) { v = core.shapeWithBidi(word, rtl, engine, shapeRun); cache.set(key, v); }
    return v;
  },
  nominal: (g) => hmtx(g),
  isBlank(g) { let v = blankCache.get(g); if (v === undefined) { v = !font.glyphToPath(g); blankCache.set(g, v); } return v; },
};
const fmt = (r) => { let s = ''; for (const g of r) s += g.join(',') + ';'; return s; };

self.onmessage = async (ev) => {
  const m = ev.data;
  try {
    await ready;
    if (m.type === 'init') {
      if (font) { font.destroy(); face.destroy(); blob.destroy(); }
      blob = hb.createBlob(m.font); face = hb.createFace(blob, 0); font = hb.createFont(face); font.setScale(1000, 1000);
      hmtx = readHmtx(m.font); script = m.script || ''; language = m.language || ''; cache = new Map(); blankCache = new Map();
      self.postMessage({ type: 'inited' });
    } else if (m.type === 'job') {
      const words = new Array(m.words.length);
      for (let i = 0; i < m.words.length; i++) {
        const glyphs = [];
        for (const it of m.bodies[i].split(';')) if (it) { const p = it.split(','); glyphs.push(p.length > 4 ? [+p[0], +p[1], +p[2], +p[3], +p[4]] : [+p[0], +p[1], +p[2], +p[3]]); }
        words[i] = { word: m.words[i], glyphs, idx: m.idx[i] };
      }
      const kinds = [], hbs = [], ids = [];
      const dump = { engine: m.engine, header: m.header, words, errors: 0 };
      let lastPost = 0;
      await core.compareDump(dump, shaper, { tolerance: m.tolerance, keepDiffering: false, maxExamples: 0,
        onProgress: (i) => { const now = Date.now(); if (now - lastPost > 100) { lastPost = now; self.postMessage({ type: 'progress', jobId: m.jobId, done: i }); } },
        onWord: (d, h, kind) => { ids.push(d.idx); kinds.push(kind === 'ok' ? 0 : kind === 'glyph' ? 1 : kind === 'width' ? 3 : 2); hbs.push(fmt(h)); } });
      self.postMessage({ type: 'result', jobId: m.jobId, ids, kinds, hbs });
    }
  } catch (e) { self.postMessage({ type: 'error', message: String(e && e.message || e), stack: e && e.stack }); }
};
