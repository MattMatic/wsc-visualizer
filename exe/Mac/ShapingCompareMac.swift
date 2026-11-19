// ShapingCompareMac.swift - dump Core Text (the shaper behind Safari/WebKit on macOS and iOS) for every word of a word list.
//
//   swiftc -O ShapingCompareMac.swift -o scmac
//   ./scmac -f Gulzara-Regular_debug.ttf -w words.txt -o Gulzara.cts.txt -R -l ur
//
// Output: "shaping dump" format 1 (see SHAPING-DUMP-SPEC.md): header lines, then one block per word:
//   <word>
//   \tct:<glyph>,<xoffset>,<yoffset>,<advance>,<rtl>;...   glyphs in VISUAL (left to right) order, 1000 units per em, rtl = 1 when the glyph's run is right-to-left
// Core Text reports glyph positions, not offsets: xoffset = position.x - (sum of the advances before the glyph), as for HarfBuzz (x = pen + offset).
// Core Text itemises scripts and applies the Unicode bidi algorithm itself (header BIDI: yes).
// Compare with HarfBuzz in the browser (dw-font-check web/compare.html or the wsc-visualizer) or with `node cli.mjs dump`; cmpct.py is no longer needed.
//
// Options:  -f font  -w words  -o out  -R (RTL paragraph)  -l lang (BCP-47, e.g. ur)  -F "liga=0,-calt"  -n N (first N words)
import Foundation
import CoreText
import CoreGraphics

var fontPath = "", wordsPath = "", outPath = "out.cts.txt", lang: String? = nil, featSpec: String? = nil
var rtl = false, limit = 0
var args = Array(CommandLine.arguments.dropFirst())
func fail(_ m: String) -> Never { FileHandle.standardError.write((m + "\n").data(using: .utf8)!); exit(1) }
while !args.isEmpty {
    let a = args.removeFirst()
    func val() -> String { if args.isEmpty { fail("missing value for \(a)") }; return args.removeFirst() }
    switch a {
    case "-f": fontPath = val()
    case "-w": wordsPath = val()
    case "-o": outPath = val()
    case "-l": lang = val()
    case "-F": featSpec = val()
    case "-n": limit = Int(val()) ?? 0
    case "-R": rtl = true
    default: fail("usage: scmac -f font.ttf -w words.txt [-o out] [-R] [-l ur] [-F features] [-n N]")
    }
}
if fontPath.isEmpty || wordsPath.isEmpty { fail("usage: scmac -f font.ttf -w words.txt [-o out] [-R] [-l ur] [-F features] [-n N]") }

// Load the font from the file (not installed, not registered). Size 1000 => units are 1/1000 em, like the other tools.
guard let fdata = NSData(contentsOfFile: fontPath), let provider = CGDataProvider(data: fdata as CFData), let cgFont = CGFont(provider)
else { fail("cannot load font \(fontPath)") }
var font = CTFontCreateWithGraphicsFont(cgFont, 1000, nil, nil)

if let spec = featSpec, !spec.isEmpty {
    var settings: [[String: Any]] = []
    for item in spec.split(separator: ",") {
        var s = item.trimmingCharacters(in: .whitespaces), v = 1
        if s.hasPrefix("+") { s.removeFirst() } else if s.hasPrefix("-") { s.removeFirst(); v = 0 }
        if let eq = s.firstIndex(of: "=") { v = Int(s[s.index(after: eq)...]) ?? 1; s = String(s[..<eq]) }
        settings.append([kCTFontOpenTypeFeatureTag as String: s, kCTFontOpenTypeFeatureValue as String: v])
    }
    let desc = CTFontDescriptorCreateCopyWithAttributes(CTFontCopyFontDescriptor(font), [kCTFontFeatureSettingsAttribute as String: settings] as CFDictionary)
    font = CTFontCreateCopyWithAttributes(font, 1000, nil, desc)
}

var baseAttrs: [NSAttributedString.Key: Any] = [NSAttributedString.Key(kCTFontAttributeName as String): font]
if let l = lang { baseAttrs[NSAttributedString.Key(kCTLanguageAttributeName as String)] = l }
if rtl {
    var d = CTWritingDirection.rightToLeft
    let ps = withUnsafeMutablePointer(to: &d) { p -> CTParagraphStyle in
        var st = CTParagraphStyleSetting(spec: .baseWritingDirection, valueSize: MemoryLayout<CTWritingDirection>.size, value: p)
        return CTParagraphStyleCreate(&st, 1)
    }
    baseAttrs[NSAttributedString.Key(kCTParagraphStyleAttributeName as String)] = ps
}

func shape(_ s: String) -> String {
    let line = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: baseAttrs))
    var out = "", pen = 0.0
    for case let run as CTRun in (CTLineGetGlyphRuns(line) as NSArray) {
        let n = CTRunGetGlyphCount(run)
        if n == 0 { continue }
        var g = [CGGlyph](repeating: 0, count: n)
        var p = [CGPoint](repeating: .zero, count: n)
        var a = [CGSize](repeating: .zero, count: n)
        CTRunGetGlyphs(run, CFRangeMake(0, 0), &g)
        CTRunGetPositions(run, CFRangeMake(0, 0), &p)
        CTRunGetAdvances(run, CFRangeMake(0, 0), &a)
        let rtlRun = CTRunGetStatus(run).contains(.rightToLeft) ? 1 : 0
        for i in 0..<n {
            out += "\(g[i]),\(Int((p[i].x - pen).rounded())),\(Int(p[i].y.rounded())),\(Int(a[i].width.rounded())),\(rtlRun);"
            pen += Double(a[i].width)
        }
    }
    return out
}

guard let text = try? String(contentsOfFile: wordsPath, encoding: .utf8) else { fail("cannot read \(wordsPath)") }
var lines = text.components(separatedBy: CharacterSet.newlines).filter { !$0.isEmpty }
if limit > 0 { lines = Array(lines.prefix(limit)) }
let fullName = (CTFontCopyName(font, kCTFontFullNameKey) as String?) ?? "", fontVers = (CTFontCopyName(font, kCTFontVersionNameKey) as String?) ?? ""
var o = "\t# FORMAT:\tshaping-dump 1\n\t# TOOL:\tscmac 1.2 (Core Text)\n\t# ENGINE:\tct\n\t# FONTFILE:\t\(fontPath)\n\t# FONTNAME:\t\(fullName)\n\t# FONTVERS:\t\(fontVers)\n"
o += "\t# WORDFILE:\t\(wordsPath)\n\t# WORDS:\t\(lines.count)\n\t# SCRIPT:\t(automatic)\n\t# LANG:\t\(lang ?? "")\n\t# LOCALE:\t\n\t# RTL:\t\(rtl ? "yes" : "no")\n\t# BIDI:\tyes\n\t# FEATURES:\t\(featSpec ?? "")\n\t# UNITS:\t1000\n"
let started = Date()
for (i, w) in lines.enumerated() {
    o += w + "\n\tct:" + shape(w) + "\n"
    if (i + 1) % 2000 == 0 || i + 1 == lines.count {
        let el = Date().timeIntervalSince(started)
        FileHandle.standardError.write(String(format: "\r%d / %d words  (%.0f s, about %.0f s left)   ", i + 1, lines.count, el, el / Double(i + 1) * Double(lines.count - i - 1)).data(using: .utf8)!)
    }
}
FileHandle.standardError.write("\n".data(using: .utf8)!)
do { try o.write(toFile: outPath, atomically: true, encoding: .utf8) } catch { fail("cannot write \(outPath)") }
print("\(lines.count) words -> \(outPath)")
