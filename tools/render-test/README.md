# render-test

Client'ın doku/sampler ayarları için ölçüm araçları (`docs/worklog/2026-10-05-anisotropic-filtering.md`).
Oyuna girmez; build'e dahil değil.

| Dosya | Ne ölçer |
|---|---|
| `sampler-reset.cpp` | D3D9Ex cihazında varsayılan sampler durumları (`MAXANISOTROPY`) ve `Reset` sonrası korunup korunmadıkları (pencere modu) |
| `sharpness.py` | İki kayıpsız ekran görüntüsünde zemin bölgelerinin ortalama gradyanı (netlik); `python sharpness.py once.png sonra.png` (Pillow gerekir) |

Derleme (`vcvars64.bat` sonrası, repo kökünden):

```bat
cl /nologo /EHsc /O2 /utf-8 /Iclient-src\extern\include tools\render-test\sampler-reset.cpp /link client-src\extern\library\DirectX\d3d9.lib user32.lib
```
