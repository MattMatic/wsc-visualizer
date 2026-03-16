# wsc-visualizer
Windows Shaping Compare Visualizer - web JavaScript viewer.

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
