// ShapingDumpWin.cpp - dump native DirectWrite (IDWriteTextAnalyzer) shaping for every word of a word list.
//
// Windows counterpart of ShapingCompareMac.swift (scmac). Writes a "shaping dump v1" file (.dws.txt), see Claude\SHAPING-DUMP-SPEC.md.
// Self-contained: plain COM, no C++/WinRT, no vcpkg, no other source files. Build (x64 Native Tools prompt):
//
//   cl /nologo /EHsc /O2 /std:c++17 /utf-8 ShapingDumpWin.cpp /Fe:scdw.exe
//
// Run:
//   scdw -f Gulzara-Regular_debug.ttf -w words.txt -o Gulzara.dws.txt -R -c ur-PK
//
// Options: -f font  -w words  -o out (default out.dws.txt)  -R right-to-left  -c locale (e.g. ur-PK)  -F "ccmp,-calt,liga=0"  -n N (first N words)
//
// Output per word: the word line, then   <TAB>dn:<glyph>,<xoffset>,<yoffset>,<advance>,<rtl>;...   in VISUAL (left to right) order, 1/1000 em.
// Script and bidi analysis as an application would run them; right-to-left runs are reversed with xoffset = -advanceOffset, runs are reordered visually.
#define NOMINMAX
#include <windows.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <algorithm>
#pragma comment(lib, "dwrite.lib")

static const char* VERSION = "1.2";
static bool g_rtl = false;

static std::wstring utf8ToUtf16(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string utf16ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
// round half away from zero, never "-0"
static int roundUnit(float v) { double r = std::round((double)v); return r == 0 ? 0 : (int)r; }

// ---- script analysis: one object is both the source and the sink of IDWriteTextAnalyzer::AnalyzeScript
struct ScriptRun { UINT32 start, length; DWRITE_SCRIPT_ANALYSIS script; };
struct BidiRun { UINT32 start, length; UINT8 level; };

class Analysis : public IDWriteTextAnalysisSource, public IDWriteTextAnalysisSink {
public:
    Analysis(const std::wstring& text, const std::wstring& locale) : ref_(1), text_(text), locale_(locale) {}
    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (!p) return E_INVALIDARG;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextAnalysisSource)) *p = static_cast<IDWriteTextAnalysisSource*>(this);
        else if (riid == __uuidof(IDWriteTextAnalysisSink)) *p = static_cast<IDWriteTextAnalysisSink*>(this);
        else { *p = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = InterlockedDecrement(&ref_); if (!n) delete this; return n; }
    // IDWriteTextAnalysisSource
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, WCHAR const** str, UINT32* len) override {
        if (pos >= text_.size()) { *str = nullptr; *len = 0; } else { *str = text_.c_str() + pos; *len = (UINT32)(text_.size() - pos); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, WCHAR const** str, UINT32* len) override {
        if (pos == 0 || pos > text_.size()) { *str = nullptr; *len = 0; } else { *str = text_.c_str(); *len = pos; }
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return g_rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 pos, UINT32* len, WCHAR const** name) override {
        *name = locale_.c_str(); *len = pos < text_.size() ? (UINT32)(text_.size() - pos) : 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 pos, UINT32* len, IDWriteNumberSubstitution** sub) override {
        *sub = nullptr; *len = pos < text_.size() ? (UINT32)(text_.size() - pos) : 0; return S_OK;
    }
    // IDWriteTextAnalysisSink
    HRESULT STDMETHODCALLTYPE SetScriptAnalysis(UINT32 pos, UINT32 len, DWRITE_SCRIPT_ANALYSIS const* sa) override {
        runs_.push_back({ pos, len, *sa }); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLineBreakpoints(UINT32, UINT32, DWRITE_LINE_BREAKPOINT const*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetBidiLevel(UINT32 pos, UINT32 len, UINT8, UINT8 resolved) override { bidi_.push_back({ pos, len, resolved }); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetNumberSubstitution(UINT32, UINT32, IDWriteNumberSubstitution*) override { return S_OK; }
    const std::vector<ScriptRun>& runs() const { return runs_; }
    const std::vector<BidiRun>& bidiRuns() const { return bidi_; }
private:
    LONG ref_; std::wstring text_, locale_; std::vector<ScriptRun> runs_; std::vector<BidiRun> bidi_;
};

struct Glyph { int gid, x, y, adv, rtl; };   // rtl: 1 when the glyph came from a right-to-left (odd level) run

static IDWriteFactory* g_factory = nullptr;
static IDWriteTextAnalyzer* g_analyzer = nullptr;
static IDWriteFontFace* g_face = nullptr;
static std::vector<DWRITE_FONT_FEATURE> g_features;
static std::wstring g_locale;

// Reorder runs into visual order (UAX #9 rule L2, applied to whole runs)
static std::vector<size_t> reorderRuns(const std::vector<int>& levels) {
    std::vector<size_t> idx(levels.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    int maxL = 0; for (int l : levels) maxL = std::max(maxL, l);
    for (int L = maxL; L > 0; L--) {
        size_t i = 0;
        while (i < idx.size()) {
            if (levels[idx[i]] >= L) { size_t j = i; while (j < idx.size() && levels[idx[j]] >= L) j++; std::reverse(idx.begin() + i, idx.begin() + j); i = j; }
            else i++;
        }
    }
    return idx;
}

// Shape one segment (one script, one bidi level). Glyphs are returned in VISUAL order inside the segment:
// a right-to-left segment (odd level) is reversed and its offset sign flipped (DirectWrite's advanceOffset has the opposite sign there).
static bool shapeSegment(const std::wstring& text, UINT32 start, UINT32 len, const DWRITE_SCRIPT_ANALYSIS& script, bool rtl, std::vector<Glyph>& out, std::string& err) {
    char buf[64];
    const WCHAR* locale = g_locale.empty() ? nullptr : g_locale.c_str();
    const WCHAR* runText = text.c_str() + start;
    DWRITE_TYPOGRAPHIC_FEATURES typo = { g_features.data(), (UINT32)g_features.size() };
    const DWRITE_TYPOGRAPHIC_FEATURES* typoPtr[1] = { &typo };
    const bool haveFeat = !g_features.empty();
    UINT32 rangeLen[1] = { len };
    UINT32 maxGlyphs = len * 5 + 16, glyphCount = 0;
    std::vector<UINT16> cluster(len), indices;
    std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProps(len);
    std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps;
    HRESULT hr;
    for (;;) {
        indices.assign(maxGlyphs, 0); glyphProps.assign(maxGlyphs, DWRITE_SHAPING_GLYPH_PROPERTIES());
        hr = g_analyzer->GetGlyphs(runText, len, g_face, FALSE, rtl ? TRUE : FALSE, &script, locale, nullptr,
                                   haveFeat ? typoPtr : nullptr, haveFeat ? rangeLen : nullptr, haveFeat ? 1 : 0,
                                   maxGlyphs, cluster.data(), textProps.data(), indices.data(), glyphProps.data(), &glyphCount);
        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && maxGlyphs < len * 64) { maxGlyphs *= 2; continue; }
        break;
    }
    if (FAILED(hr)) { snprintf(buf, sizeof buf, "DirectWrite hr=0x%08lX", (unsigned long)hr); err = buf; return false; }
    indices.resize(glyphCount); glyphProps.resize(glyphCount);
    std::vector<FLOAT> adv(glyphCount); std::vector<DWRITE_GLYPH_OFFSET> off(glyphCount);
    hr = g_analyzer->GetGlyphPlacements(runText, cluster.data(), textProps.data(), len, indices.data(), glyphProps.data(), glyphCount,
                                        g_face, 1000.0f, FALSE, rtl ? TRUE : FALSE, &script, locale,
                                        haveFeat ? typoPtr : nullptr, haveFeat ? rangeLen : nullptr, haveFeat ? 1 : 0, adv.data(), off.data());
    if (FAILED(hr)) { snprintf(buf, sizeof buf, "DirectWrite hr=0x%08lX", (unsigned long)hr); err = buf; return false; }
    for (UINT32 i = 0; i < glyphCount; i++) out.push_back({ (int)indices[i], roundUnit(off[i].advanceOffset), roundUnit(off[i].ascenderOffset), roundUnit(adv[i]), rtl ? 1 : 0 });
    if (rtl) {
        std::reverse(out.begin(), out.end());
        for (Glyph& g : out) g.x = g.x == 0 ? 0 : -g.x;
    }
    return true;
}

// Returns false and fills err on failure.
// Like an application using DirectWrite: script analysis AND bidi analysis (IDWriteTextAnalyzer::AnalyzeBidi), every script/bidi run is shaped
// with isRightToLeft = odd level, and the runs are put into visual order. (Version 1.0 shaped the whole word as one right-to-left run,
// which reversed digit groups such as "9،975".)
static bool shapeWord(const std::string& utf8, std::vector<Glyph>& out, std::string& err) {
    out.clear();
    std::wstring text = utf8ToUtf16(utf8);
    UINT32 textLen = (UINT32)text.size();
    Analysis* an = new Analysis(text, g_locale);
    char buf[64];
    HRESULT hr = g_analyzer->AnalyzeScript(static_cast<IDWriteTextAnalysisSource*>(an), 0, textLen, static_cast<IDWriteTextAnalysisSink*>(an));
    if (SUCCEEDED(hr)) hr = g_analyzer->AnalyzeBidi(static_cast<IDWriteTextAnalysisSource*>(an), 0, textLen, static_cast<IDWriteTextAnalysisSink*>(an));
    if (FAILED(hr)) { an->Release(); snprintf(buf, sizeof buf, "DirectWrite hr=0x%08lX", (unsigned long)hr); err = buf; return false; }
    std::vector<UINT32> cuts = { 0, textLen };
    for (const ScriptRun& r : an->runs()) { cuts.push_back(r.start); cuts.push_back(r.start + r.length); }
    for (const BidiRun& r : an->bidiRuns()) { cuts.push_back(r.start); cuts.push_back(r.start + r.length); }
    std::sort(cuts.begin(), cuts.end()); cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<std::vector<Glyph>> segs; std::vector<int> levels;
    for (size_t k = 0; k + 1 < cuts.size(); k++) {
        UINT32 a = cuts[k], b = cuts[k + 1];
        if (a >= textLen || b <= a) continue;
        const ScriptRun* sr = nullptr; for (const ScriptRun& r : an->runs()) if (a >= r.start && a < r.start + r.length) { sr = &r; break; }
        if (!sr) continue;
        int level = g_rtl ? 1 : 0; for (const BidiRun& r : an->bidiRuns()) if (a >= r.start && a < r.start + r.length) { level = r.level; break; }
        std::vector<Glyph> g;
        if (!shapeSegment(text, a, b - a, sr->script, (level & 1) != 0, g, err)) { an->Release(); return false; }
        segs.push_back(std::move(g)); levels.push_back(level);
    }
    an->Release();
    for (size_t i : reorderRuns(levels)) out.insert(out.end(), segs[i].begin(), segs[i].end());
    return true;
}

// ---- font names (name table: nameID 4 full name, 5 version)
static std::string nameString(const uint8_t* t, size_t size, int wantId) {
    auto u16 = [&](size_t o) -> unsigned { return o + 1 < size ? (t[o] << 8) | t[o + 1] : 0; };
    unsigned count = u16(2), strOff = u16(4);
    std::string best; int bestRank = -1;
    for (unsigned i = 0; i < count; i++) {
        size_t o = 6 + 12 * (size_t)i;
        unsigned plat = u16(o), id = u16(o + 6), len = u16(o + 8), off = u16(o + 10);
        if ((int)id != wantId || strOff + off + len > size) continue;
        int rank = plat == 3 ? 2 : plat == 0 ? 1 : 0;
        if (rank <= bestRank) continue;
        std::string s;
        if (plat == 3 || plat == 0) { std::wstring w; for (unsigned k = 0; k + 1 < len; k += 2) w.push_back((wchar_t)u16(strOff + off + k)); s = utf16ToUtf8(w); }
        else s.assign((const char*)t + strOff + off, len);
        best = s; bestRank = rank;
    }
    return best;
}

// ---- feature list: "ccmp,-calt,liga=0,+kern"
static bool parseFeatures(const std::string& spec, std::vector<DWRITE_FONT_FEATURE>& out) {
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t end = spec.find(',', pos); if (end == std::string::npos) end = spec.size();
        std::string item = spec.substr(pos, end - pos); pos = end + 1;
        size_t a = item.find_first_not_of(" \t"); if (a == std::string::npos) { if (end >= spec.size()) break; continue; }
        item = item.substr(a, item.find_last_not_of(" \t") - a + 1);
        UINT32 value = 1;
        if (item[0] == '+') item.erase(0, 1); else if (item[0] == '-') { value = 0; item.erase(0, 1); }
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string v = item.substr(eq + 1); item.erase(eq);
            if (v == "on") value = 1; else if (v == "off") value = 0; else if (!v.empty() && std::all_of(v.begin(), v.end(), ::isdigit)) value = (UINT32)std::stoul(v); else return false;
        }
        if (item.empty() || item.size() > 4) return false;
        while (item.size() < 4) item.push_back(' ');
        DWRITE_FONT_FEATURE f; f.nameTag = (DWRITE_FONT_FEATURE_TAG)((UINT32)(uint8_t)item[0] | ((UINT32)(uint8_t)item[1] << 8) | ((UINT32)(uint8_t)item[2] << 16) | ((UINT32)(uint8_t)item[3] << 24));
        f.parameter = value; out.push_back(f);
        if (end >= spec.size()) break;
    }
    return true;
}

static std::string readFile(const char* path, bool& ok) {
    FILE* f = nullptr; ok = false;
    if (fopen_s(&f, path, "rb") || !f) return std::string();
    std::string s; char buf[1 << 16]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f); ok = true; return s;
}
static int fail(const char* m) { fprintf(stderr, "%s\n", m); return 1; }

int main(int argc, char** argv) {
    std::string fontPath, wordsPath, outPath = "out.dws.txt", features, locale;
    int limit = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(1); } return argv[++i]; };
        if (a == "-f") fontPath = val(); else if (a == "-w") wordsPath = val(); else if (a == "-o") outPath = val();
        else if (a == "-c") locale = val(); else if (a == "-F") features = val(); else if (a == "-n") limit = atoi(val().c_str());
        else if (a == "-R") g_rtl = true;
        else return fail("usage: scdw -f font.ttf -w words.txt [-o out.dws.txt] [-R] [-c ur-PK] [-F features] [-n N]");
    }
    if (fontPath.empty() || wordsPath.empty()) return fail("usage: scdw -f font.ttf -w words.txt [-o out.dws.txt] [-R] [-c ur-PK] [-F features] [-n N]");
    fprintf(stderr, "scdw %s (DirectWrite native)\n", VERSION);
    g_locale = utf8ToUtf16(locale);
    if (!features.empty() && !parseFeatures(features, g_features)) return fail("bad -F feature list");

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_factory)))) return fail("cannot create DirectWrite factory");
    if (FAILED(g_factory->CreateTextAnalyzer(&g_analyzer))) return fail("cannot create text analyzer");
    std::wstring fontW = utf8ToUtf16(fontPath);
    IDWriteFontFile* file = nullptr;
    if (FAILED(g_factory->CreateFontFileReference(fontW.c_str(), nullptr, &file))) return fail("cannot open font file");
    BOOL supported = FALSE; DWRITE_FONT_FILE_TYPE ft; DWRITE_FONT_FACE_TYPE fct; UINT32 nfaces = 0;
    file->Analyze(&supported, &ft, &fct, &nfaces);
    if (!supported) return fail("font file type not supported by DirectWrite");
    IDWriteFontFile* files[] = { file };
    if (FAILED(g_factory->CreateFontFace(fct, 1, files, 0, DWRITE_FONT_SIMULATIONS_NONE, &g_face))) return fail("cannot create font face");

    std::string fullName, version;
    {
        const void* data = nullptr; UINT32 size = 0; void* ctx = nullptr; BOOL exists = FALSE;
        if (SUCCEEDED(g_face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('n', 'a', 'm', 'e'), &data, &size, &ctx, &exists)) && exists) {
            fullName = nameString((const uint8_t*)data, size, 4); version = nameString((const uint8_t*)data, size, 5);
            g_face->ReleaseFontTable(ctx);
        }
    }

    bool ok; std::string text = readFile(wordsPath.c_str(), ok);
    if (!ok) return fail("cannot read word list");
    std::vector<std::string> words; size_t p = 0;
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) p = 3;
    while (p < text.size()) {
        size_t e = text.find_first_of("\r\n", p); if (e == std::string::npos) e = text.size();
        if (e > p) words.push_back(text.substr(p, e - p));
        p = e + 1;
    }
    if (limit > 0 && (size_t)limit < words.size()) words.resize(limit);

    FILE* o = nullptr;
    if (fopen_s(&o, outPath.c_str(), "wb") || !o) return fail("cannot write output file");
    static char big[1 << 20]; setvbuf(o, big, _IOFBF, sizeof big);
    fprintf(o, "\t# FORMAT:\tshaping-dump 1\n\t# TOOL:\tscdw %s (DirectWrite IDWriteTextAnalyzer)\n\t# ENGINE:\tdn\n\t# FONTFILE:\t%s\n\t# FONTNAME:\t%s\n\t# FONTVERS:\t%s\n",
            VERSION, fontPath.c_str(), fullName.c_str(), version.c_str());
    fprintf(o, "\t# WORDFILE:\t%s\n\t# WORDS:\t%zu\n\t# SCRIPT:\t(automatic)\n\t# LANG:\t\n\t# LOCALE:\t%s\n\t# RTL:\t%s\n\t# BIDI:\tyes\n\t# FEATURES:\t%s\n\t# UNITS:\t1000\n",
            wordsPath.c_str(), words.size(), locale.c_str(), g_rtl ? "yes" : "no", features.c_str());

    auto t0 = std::chrono::steady_clock::now();
    std::vector<Glyph> gl; size_t errors = 0;
    for (size_t i = 0; i < words.size(); i++) {
        std::string err;
        fprintf(o, "%s\n", words[i].c_str());
        if (shapeWord(words[i], gl, err)) {
            fputs("\tdn:", o);
            for (const Glyph& g : gl) fprintf(o, "%d,%d,%d,%d,%d;", g.gid, g.x, g.y, g.adv, g.rtl);
            fputc('\n', o);
        } else { fprintf(o, "\tdn!%s\n", err.c_str()); errors++; }
        if ((i + 1) % 2000 == 0 || i + 1 == words.size()) {
            double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "\r%zu / %zu words  (%.0f s, about %.0f s left)   ", i + 1, words.size(), el, el / (double)(i + 1) * (double)(words.size() - i - 1));
        }
    }
    fputc('\n', stderr);
    fclose(o);
    printf("%zu words -> %s%s\n", words.size(), outPath.c_str(), errors ? " (some words failed, see dn! lines)" : "");
    g_face->Release(); file->Release(); g_analyzer->Release(); g_factory->Release();
    return 0;
}
