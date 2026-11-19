# Build Windows DirectWrite Shaping
Easiest: Start menu → search “x64 Native Tools Command Prompt for VS” (it may say “for VS 18” or “2026”).

```
cd /d D:\Matt\VisualStudio\R&D\WindowsShapingCompare\ShapingCompare\Win
cl /nologo /EHsc /O2 /std:c++17 /utf-8 ShapingDumpWin.cpp /Fe:scdw.exe
```