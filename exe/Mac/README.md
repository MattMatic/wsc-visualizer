# ShapingCompareMac (scmac) - Core Text (Safari / WebKit) shaping dump

Core Text is the shaping engine behind Safari and WebKit on macOS and iOS. `scmac` dumps what Core Text does with every word of a word list into a
**`.cts.txt` shaping dump** (format: `Claude\SHAPING-DUMP-SPEC.md`). The Windows counterpart is `scdw` (`Win\ShapingDumpWin.cpp`, `.dws.txt`).
The dumps contain engine output only; the comparison with HarfBuzz is done in the browser (no Python needed):

* **dw-font-check** `web/compare.html`, or the "Measured results" box of the main page (runs as part of Analyze), or `node cli.mjs dump font.ttf a.cts.txt b.dws.txt`
* **wsc-visualizer**: drop the font and the `.cts.txt` (and/or `.dws.txt`) to see HarfBuzz, DirectWrite and Core Text side by side.

## 1. Build and run (on a Mac)

```
swiftc -O ShapingCompareMac.swift -o scmac
./scmac -f Gulzara-Regular.ttf -w 20260928-Urdu_PJN_words_punct_numbers.txt -o Gulzara-Regular.cts.txt -R -l ur
```

| Option | Meaning |
|---|---|
| `-f` | font file (loaded from the file; no need to install it) |
| `-w` | UTF-8 word list, one word per line |
| `-o` | output file (default `out.cts.txt`) |
| `-R` | right-to-left paragraph (use for Urdu/Arabic) |
| `-l` | language tag passed to Core Text, e.g. `ur` |
| `-F` | OpenType features, e.g. `"liga=0,-calt"` |
| `-n` | only the first N words (quick test) |

A progress counter with a time estimate is printed to stderr every 2,000 words.

Output: header lines (`FORMAT`, `TOOL`, `FONTNAME`, `FONTVERS`, `WORDFILE`, `RTL`, `BIDI`, ...), then for each word the word on one line and
`\tct:glyph,xoffset,yoffset,advance,rtl;...` on the next. Glyphs are in visual (left to right) order, 1000 units per em. Core Text reports glyph positions, not offsets,
so the offset is position.x minus the sum of the advances before the glyph (the same model as HarfBuzz, x = pen + offset). `rtl` is 1 for glyphs of a right-to-left run.
Core Text runs the Unicode bidi algorithm and itemises scripts itself, so no extra handling is needed in the tool (header `BIDI: yes`).

## 2. Compare with HarfBuzz (optional Python tool)

`cmpct.py` still works on `.cts.txt` / old `.ct.txt` files (it ignores the extra `rtl` field) but is no longer needed:
```
python3 -m pip install uharfbuzz fonttools
python3 cmpct.py Gulzara-Regular.cts.txt Gulzara-Regular.ttf --rtl -o Gulzara.ct.wsc.txt
```

| Option | Meaning |
|---|---|
| `--rtl` | HarfBuzz shapes right to left (use when the dump was made with `-R`) |
| `--script`, `--lang` | HarfBuzz script and language (defaults `arab`, `urd`) |
| `--features` | same feature syntax as `-F`; use the same value as for the dump |
| `--tol` | tolerance in units (default 2) |
| `--width` | also compare total width (right edge of the rightmost base glyph) |
| `--blanks` | also compare the offsets of blank glyphs (space, ZWJ ...) |
| `-o` | write the differing words in `.wsc` style (`hb:` and `ct#` lines) |

A word differs when the glyph ids differ or a non-blank glyph's drawn position (pen + offset) differs by more than the tolerance. Total width is
not compared by default: Core Text gives attached marks a negative advance and HarfBuzz a zero advance plus an offset, so sums of advances differ
even when every glyph is drawn in the same place.

### If `pip install` succeeds but Python says "No module named 'uharfbuzz'"
`pip` and `python3` are different installs. Use `python3 -m pip install ...` (same interpreter). With Homebrew Python
("externally-managed-environment") use a virtual environment:

```
python3 -m venv ~/sc-venv && source ~/sc-venv/bin/activate
pip install uharfbuzz fonttools
python cmpct.py ...
```

## Versions and progress
`scmac` 1.2 writes the format-1 header (`TOOL: scmac 1.2`); check it to see which build made a dump. `cmpct.py` prints a version banner when it starts.
`cmpct.py` 1.0 reversed HarfBuzz's glyph order twice (wrong results, ~56% match); 1.1 fixes that.

## Limits
* Safari/WebKit use Core Text but add their own handling (default features, kerning, fallback), so a match here is strong evidence, not proof.
  Check the final result visually in Safari.
* iOS uses the same engine but the Core Text version can differ from macOS; test on a real device for a final check.
* Core Text has its own OpenType and `morx` handling and is not a clean OpenType implementation. Differences from HarfBuzz may need their own probes
  (the `Probes\P*.ttf` fonts can be run through the same two steps).
* The Swift file has not been compiled by its author on every macOS version; if `kCTFontOpenTypeFeatureTag` is unavailable on an older macOS,
  drop the `-F` option support.

## Viewing the results

In the `wsc-visualizer` web page, drag the `.cts.txt` dump (or the `.ct.wsc.txt` file written by `cmpct.py` 1.1 or later) and the same `.ttf` onto it.
It shows HarfBuzz beside a Core Text column, with the glyph table and the differing
values highlighted. Core Text is compared on drawn positions (see Limits).
Files made by `cmpct.py` 1.0 are rejected by the visualiser because they used the wrong
comparison; re-run 1.1 or later.

## Version 1.2: bidi

Core Text runs the Unicode bidirectional algorithm; HarfBuzz does not. For `--rtl`, `cmpct.py` 1.2 splits each word into bidi runs
(digits, Latin and numbers next to digits form left-to-right runs inside the right-to-left paragraph), shapes each run
separately and joins them in visual order, as Core Text does. Without this, number strings such as `12` looked wrong
because HarfBuzz returned them reversed. The bidi code is a small subset of UAX #9 (enough for Arabic letters, digits, Latin and punctuation).

## Running the probe fonts on Core Text

`RunProbesMac.sh` is the Mac counterpart of `RunProbes.bat`: it shapes every probe font in `Probes` with Core Text, compares with HarfBuzz and prints PASS/FAIL.

```
chmod +x RunProbesMac.sh
./RunProbesMac.sh ../Probes P26        # only the P26 probes (a zero-advance mark between cursively joined letters)
./RunProbesMac.sh ../Probes            # all probes
```

The table is also written to `ProbeSummaryMac.txt`. Open a failing `<probe>.ct.wsc.txt` in the wsc-visualizer (with the probe `.ttf`) to see where the mark goes.
