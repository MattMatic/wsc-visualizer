// Shaping dump files (.cts.txt Core Text, .dws.txt DirectWrite): parser, bidi, drawn positions, comparison (shaper-agnostic: no HarfBuzz, no DOM, no other imports).
// SINGLE SOURCE: dw-font-check/src/shapedump-core.mjs; wsc-visualizer/js/shapedump-core.mjs is a copy (keep in sync).
// Format: Claude/SHAPING-DUMP-SPEC.md (v1; v0 = older scmac .ct.txt). No DOM, works in Node and in the browser (needs harfbuzzjs via hb.mjs).

// ---------------------------------------------------------------- parsing
/** -> { header:{KEY:value}, engine:'ct'|'dn'|null, format:number, words:[{word, glyphs:[[gid,x,y,adv]...]|null, error:string|null}], errors:number } */
export function parseDump(text) {
  const header = {}, words = []; let cur = null, engine = null, errors = 0;
  const parseBody = (b) => { const r = []; for (const it of b.split(';')) if (it) { const p = it.split(','); r.push(p.length > 4 ? [+p[0], +p[1], +p[2], +p[3], +p[4]] : [+p[0], +p[1], +p[2], +p[3]]); } return r; };
  let pos = text.charCodeAt(0) === 0xFEFF ? 1 : 0;
  while (pos < text.length) {
    let e = text.indexOf('\n', pos); if (e < 0) e = text.length;
    let end = e; if (end > pos && text.charCodeAt(end - 1) === 13) end--;
    if (end > pos) {
      const c0 = text.charCodeAt(pos);
      if (c0 !== 9) cur = text.slice(pos, end);
      else if (text.charCodeAt(pos + 1) === 35) { // "\t#"
        const m = /^\t#\s*([A-Z]+):\t?(.*)$/.exec(text.slice(pos, end)); if (m && cur === null) header[m[1]] = m[2].trim();
      } else if (cur !== null) {
        const m = /^\t(ct|dn)([:!])/.exec(text.slice(pos, pos + 4));
        if (m) {
          engine ||= m[1];
          if (m[2] === ':') words.push({ word: cur, glyphs: parseBody(text.slice(pos + 4, end)), error: null });
          else { words.push({ word: cur, glyphs: null, error: text.slice(pos + 4, end) }); errors++; }
          cur = null;
        }
      }
    }
    pos = e + 1;
  }
  if (header.ENGINE) engine = header.ENGINE;
  const fm = /(\d+)\s*$/.exec(header.FORMAT || '');
  return { header, engine, format: fm ? +fm[1] : 0, words, errors };
}

// ---------------------------------------------------------------- bidi (port of cmpct.py 1.3: small UAX #9 subset for an RTL paragraph)
const RE = {
  arabicLetter: /[\p{Script=Arabic}\p{Script=Syriac}\p{Script=Thaana}\p{Script=Mandaic}]/u,
  rLetter: /[\p{Script=Hebrew}\p{Script=Nko}\p{Script=Samaritan}]/u,
  letter: /\p{L}|\p{Nl}/u, mark: /[\p{Mn}\p{Me}]/u, format: /\p{Cf}/u, currency: /\p{Sc}/u,
};
export function bidiClass(cp) {
  if (cp === 0x200F) return 'R'; if (cp === 0x200E) return 'L'; if (cp === 0x061C) return 'AL';
  if ((cp >= 0x30 && cp <= 0x39) || (cp >= 0x6F0 && cp <= 0x6F9)) return 'EN';
  if ((cp >= 0x660 && cp <= 0x669) || cp === 0x66B || cp === 0x66C) return 'AN';
  if (cp === 0x2B || cp === 0x2D) return 'ES';
  if (cp === 0x2C || cp === 0x2E || cp === 0x2F || cp === 0x3A || cp === 0xA0 || cp === 0x60C) return 'CS';
  if (cp === 0x23 || cp === 0x24 || cp === 0x25 || cp === 0xB0 || cp === 0xB1 || cp === 0x66A || (cp >= 0x2030 && cp <= 0x2034)) return 'ET';
  const c = String.fromCodePoint(cp);
  if (RE.currency.test(c)) return 'ET';
  if (RE.mark.test(c)) return 'NSM';
  if (RE.format.test(c)) return 'BN';
  if (RE.arabicLetter.test(c) && RE.letter.test(c)) return 'AL';
  if (RE.rLetter.test(c) && RE.letter.test(c)) return 'R';
  if (RE.letter.test(c)) return 'L';
  return 'ON';
}
/** Levels (1 or 2) of each code point of an RTL paragraph; `cps` = array of code point strings. */
export function bidiLevels(cps) {
  let t = cps.map((c) => bidiClass(c.codePointAt(0)));
  const n = t.length;
  for (let i = 0; i < n; i++) if (t[i] === 'NSM' || t[i] === 'BN') t[i] = i ? t[i - 1] : 'R';                       // W1
  const prevStrong = (i, kinds) => { for (let k = i - 1; k >= 0; k--) if (kinds.includes(t[k])) return t[k]; return 'R'; };
  for (let i = 0; i < n; i++) if (t[i] === 'EN' && prevStrong(i, ['R', 'L', 'AL']) === 'AL') t[i] = 'AN';           // W2
  t = t.map((c) => (c === 'AL' ? 'R' : c));                                                                         // W3
  for (let i = 0; i < n; i++) if ((t[i] === 'ES' || t[i] === 'CS') && i > 0 && i < n - 1 && t[i - 1] === t[i + 1] && (t[i - 1] === 'EN' || t[i - 1] === 'AN')) t[i] = t[i - 1]; // W4
  for (let i = 0; i < n; i++) if (t[i] === 'ET') {                                                                  // W5
    let j = i; while (j < n && t[j] === 'ET') j++;
    let k = i; while (k >= 0 && t[k] === 'ET') k--;
    if ((j < n && t[j] === 'EN') || (k >= 0 && t[k] === 'EN')) for (let m = k + 1; m < j; m++) t[m] = 'EN';
  }
  t = t.map((c) => (c === 'ES' || c === 'ET' || c === 'CS' ? 'ON' : c));                                           // W6
  for (let i = 0; i < n; i++) if (t[i] === 'EN' && prevStrong(i, ['R', 'L']) === 'L') t[i] = 'L';                  // W7
  const strong = (c) => (c === 'L' ? 'L' : (c === 'R' || c === 'EN' || c === 'AN') ? 'R' : null);
  const lv = new Array(n).fill(null);
  for (let i = 0; i < n; i++) if (strong(t[i])) lv[i] = t[i];
  for (let i = 0; i < n; i++) if (lv[i] === null) {                                                                 // N1/N2
    let p = 'R', q = 'R';
    for (let k = i - 1; k >= 0; k--) { const s = strong(t[k]); if (s) { p = s; break; } }
    for (let k = i + 1; k < n; k++) { const s = strong(t[k]); if (s) { q = s; break; } }
    lv[i] = p === q ? p : 'R';
  }
  return t.map((c, i) => (c === 'EN' || c === 'AN' || lv[i] === 'L' ? 2 : 1));                                      // I1/I2, base level 1
}
export function reorder(levels) {
  const idx = levels.map((_, i) => i);
  for (let L = Math.max(...levels); L > 0; L--) {
    let i = 0;
    while (i < idx.length) {
      if (levels[idx[i]] >= L) { let j = i; while (j < idx.length && levels[idx[j]] >= L) j++; const seg = idx.slice(i, j).reverse(); for (let k = i; k < j; k++) idx[k] = seg[k - i]; i = j; } else i++;
    }
  }
  return idx;
}

/**
 * Shape one word the way the engine's itemisation requires, using shapeRun(text, rtl, itemise) -> glyph tuples (visual order).
 * rtl=false: one LTR run. engine 'dn': the whole word as one RTL run (no bidi in DirectWrite's native analyzer).
 * engine 'ct': Core Text's bidi runs (cmpct.py 1.3 subset), runs joined in visual order, Latin runs itemised.
 */
export function shapeWithBidi(word, rtl, engine, shapeRun) {
  if (!rtl) return shapeRun(word, false, engine === 'ct');
  if (engine === 'dn') return shapeRun(word, true, false);
  const cps = Array.from(word), lv = bidiLevels(cps), runs = [];
  cps.forEach((c, i) => { if (runs.length && runs[runs.length - 1][0] === lv[i]) runs[runs.length - 1][1] += c; else runs.push([lv[i], c]); });
  if (runs.length === 1) return shapeRun(runs[0][1], runs[0][0] % 2 === 1, true);
  const out = [];
  for (const k of reorder(runs.map((r) => r[0]))) out.push(...shapeRun(runs[k][1], runs[k][0] % 2 === 1, true));
  return out;
}
export function parseFeatureString(spec) {
  const out = [];
  for (let item of String(spec || '').split(',')) {
    item = item.trim(); if (!item) continue; let v = 1;
    if (item[0] === '+') item = item.slice(1); else if (item[0] === '-') { item = item.slice(1); v = 0; }
    const eq = item.indexOf('=');
    if (eq >= 0) { const s = item.slice(eq + 1); item = item.slice(0, eq); v = s === 'on' ? 1 : s === 'off' ? 0 : parseInt(s, 10) || 0; }
    out.push({ tag: item, value: v });
  }
  return out;
}

// ---------------------------------------------------------------- drawn positions and comparison
/** Drawn positions: [[gid, x, y]...] and the right edge (max x+adv over glyphs with adv>0). `dnRtl`: DirectWrite right-to-left correction (adv - nominal). */
export function drawn(glyphs, nominal = null) {
  let pen = 0, edge = 0; const o = [];
  for (const [g, dx, dy, adv, rt] of glyphs) {
    const x = pen + dx + (nominal && (rt === undefined || rt === 1) ? adv - nominal(g) : 0);   // DirectWrite correction only for right-to-left runs (5th field; absent = paragraph direction)
    o.push([g, x, dy]);
    if (adv > 0) edge = Math.max(edge, pen + dx + adv);
    pen += adv;
  }
  return { o, edge, pen };
}

/**
 * Compare a parsed dump with HarfBuzz.
 * options: { tolerance:2, blanks:false, width:false, keepDiffering:true, rtl: (header RTL), maxExamples:8, onProgress(i,n), glyphNames:(gid)=>string, shouldStop() }
 * -> { engine, total, differingWords:[word], errors, ok, kinds:{glyph,width,position}, differing:[{word, hb, eng, kind}], examples:{kind:[words]},
 *      glyphMisplaced:Map<gid,count>, maxDelta:{x,y}, deltaHist:{le5,le20,le100,gt100}, distinctWords, rtl, tolerance }
 */
export async function compareDump(dump, shaper, options = {}) {
  const tol = options.tolerance ?? 2, engine = dump.engine || 'ct';
  const rtl = options.rtl ?? /^y/i.test(dump.header.RTL || '');
  // DirectWrite dumps made with BIDI: yes (scdw >= 1.1) were shaped per bidi run like Core Text; older dumps shaped the whole word as one RTL run
  const shapeEngine = engine === 'dn' && !/^y/i.test(dump.header.BIDI || '') ? 'dn' : 'ct';
  const dnFix = engine === 'dn' && rtl, nominal = dnFix ? (g) => shaper.nominal(g) : null;
  const res = { engine, rtl, tolerance: tol, total: dump.words.length, errors: dump.errors, ok: 0, kinds: { glyph: 0, width: 0, position: 0 },
    differing: [], differingWords: [], examples: { glyph: [], width: [], position: [] }, glyphMisplaced: new Map(), deltaHist: { le5: 0, le20: 0, le100: 0, gt100: 0 }, maxDelta: { x: 0, y: 0 }, stopped: false };
  const maxEx = options.maxExamples ?? 8;
  const t0 = Date.now(); let lastYield = t0;
  const yieldNow = typeof setTimeout === 'function' ? () => new Promise((r) => setTimeout(r, 0)) : null;
  for (let i = 0; i < dump.words.length; i++) {
    const d = dump.words[i];
    if ((i & 127) === 0) {                      // let the browser paint and answer clicks (every ~80 ms)
      const now = Date.now();
      if (options.onProgress) options.onProgress(i + 1, dump.words.length, (now - t0) / 1000);
      if (yieldNow && now - lastYield > 80) { await yieldNow(); lastYield = Date.now(); }
      if (options.shouldStop && options.shouldStop()) { res.stopped = true; break; }
    }
    if (!d.glyphs) continue;
    let h; try { h = shaper.shape(d.word, rtl, shapeEngine); } catch { res.errors++; continue; }
    let kind = 'ok';
    if (h.length !== d.glyphs.length || h.some((x, k) => x[0] !== d.glyphs[k][0])) kind = 'glyph';
    else {
      const a = drawn(d.glyphs, nominal), b = drawn(h);
      if (options.width && Math.abs(a.edge - b.edge) > tol) kind = 'width';
      for (let k = 0; k < a.o.length; k++) {
        const g = a.o[k][0]; if (!options.blanks && shaper.isBlank(g)) continue;
        const dx = Math.abs(a.o[k][1] - b.o[k][1]), dy = Math.abs(a.o[k][2] - b.o[k][2]);
        if (dx > tol || dy > tol) {
          if (kind === 'ok') kind = 'position';
          res.glyphMisplaced.set(g, (res.glyphMisplaced.get(g) || 0) + 1);
          const m = Math.max(dx, dy); res.deltaHist[m <= 5 ? 'le5' : m <= 20 ? 'le20' : m <= 100 ? 'le100' : 'gt100']++;
          if (dx > res.maxDelta.x) res.maxDelta.x = dx; if (dy > res.maxDelta.y) res.maxDelta.y = dy;
        }
      }
    }
    if (options.onWord) options.onWord(d, h, kind);
    if (kind === 'ok') res.ok++;
    else { res.kinds[kind]++; if (res.examples[kind].length < maxEx) res.examples[kind].push(d.word); res.differingWords.push(d.word); if (options.keepDiffering !== false) res.differing.push({ word: d.word, hb: h, eng: d.glyphs, kind }); }
  }
  res.compared = res.total - res.errors;
  return res;
}

