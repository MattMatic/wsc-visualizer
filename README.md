# wsc-visualizer
Shaping Compare Visualizer - web JavaScript viewer.

# Live
https://mattmatic.github.io/wsc-visualizer

# Concept
Needed a tool to compare the complex script shaping between:
- HarfBuzz
- DirectWrite
- Uniscribe

A relatively small Windows EXE file does the hard work of actually shaping from a word-list
and a font. It produces a `diff.wsc.txt` output file that has the words and the glyph output
data for each word.

This client-sided web tool will let you load the font and the wsc.txt file to see how the
glyphs actually look.

# Running the EXE
Some examples:

```
ShapingCompare.exe -ADU --rtl --font="C:\fonts\Gulzara.ttf" --words="C:\data\UrduWords.txt" --out="Gulzara-urdu.wsc.txt"

ShapingCompare.exe -D --font="C:\fonts\NotoSansBengali.ttf" --words="C:\data\BengaliWords.txt" --out="NotoSansBengali.wsc.txt"
```

- `-ADU` will output all words, including matches, and render with HarfBuzz, DirectWrite, and Uniscribe
- `-D` will output only HarfBuzz and DirectWrite, with the output containing only differences.

# Raw shaping dumps (Core Text and DirectWrite)
Besides `*.wsc.txt`, the viewer accepts raw dumps made by the small dump tools: `scdw` (Windows, DirectWrite, `*.dws.txt`) and `scmac` (macOS, Core Text, `*.cts.txt`).
Drop the font and one or both dumps (any order). The page shapes every dump word with HarfBuzz in the browser and compares the drawn glyph positions, then shows the usual
HarfBuzz / DirectWrite / Core Text columns. All words are kept, so the *Same* filter works as well as *Different*. Set the script/language boxes if HarfBuzz should not guess (Urdu: `arab`, `urd`).
Format: `SHAPING-DUMP-SPEC.md` (dw-font-check / ShapingCompare). `js/shapedump-core.mjs` is a copy of `dw-font-check/src/shapedump-core.mjs`.

## Speed: worker pool and HarfBuzz cache (dumps)
Dump comparison runs in up to 7 web workers (`js/dumpworker.js`, each with its own HarfBuzz). A word is always sent to the same worker (hash of the word), and every worker caches its
HarfBuzz results per word, paragraph direction and itemisation mode. Loading a second dump of the same corpus (for example the other engine, or a re-dump after a tool fix) therefore
does not reshape the words. Dropping a new font, or changing the script/language boxes, restarts the workers and clears the caches.
