# font-compare

Yazı çizimi kararının (`docs/worklog/2026-10-05-font-gdi-render.md`) ölçüm araçları. Oyuna girmez, derlenmez;
sadece sonucu yeniden üretmek için.

| Dosya | Ne ölçer |
|---|---|
| `glyph-compare.cpp` | Orijinal client / Anka2 yöntemi (GDI + 1-bit eşik) ile FreeType ayarlarının harf piksellerini ve ilerleme genişliklerini karşılaştırır |
| `cell-overhang.cpp` | GDI'ın harfi, orijinal kodun eşiklediği hücrenin dışına çizip çizmediğini sayar |
| `new-vs-original.cpp` | `GrpFontTexture`'ın yeni çizimi (sol pay + `bearingX`, `TRANSPARENT`) ekranda orijinal/Anka2 ile aynı pikselleri mi veriyor; hücre dışına çizim var mı |

Derleme (Developer PowerShell ya da `vcvars64.bat` sonrası, repo kökünden):

```bat
cl /nologo /EHsc /O2 /MT /utf-8 /Iclient-src\vendor\freetype-2.13.3\include tools\font-compare\glyph-compare.cpp /link <freetype.lib> gdi32.lib user32.lib
cl /nologo /EHsc /O2 /utf-8 tools\font-compare\cell-overhang.cpp /link gdi32.lib user32.lib
cl /nologo /EHsc /O2 /utf-8 tools\font-compare\new-vs-original.cpp /link gdi32.lib user32.lib
```

`glyph-compare` FreeType kaynağını ve derlenmiş `freetype.lib`'i ister (client build'inden çıkarıldı; gerekirse
`client-src/vendor/freetype-2.13.3`'ten ayrıca derlenir). Fontları `C:\Windows\Fonts`'tan okur.
