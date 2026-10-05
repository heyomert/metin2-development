# config-test

`config.exe` ve oyunun ayar kodu için ölçüm/test programları (`docs/worklog/2026-10-05-config-tool.md`).
Oyuna girmez; build'e dahil değil.

| Dosya | Ne ölçer |
|---|---|
| `roundtrip.cpp` | `client-src/src/Config/ConfigFile` ile yazılan dosyayı oyunun `LoadConfig` döngüsünün kopyasıyla okur; değerler, korunan anahtarlar, eski `config.exe` dosyası, boş satır |
| `fullscreen-refresh.cpp` | Oyunun tam ekran `CreateDeviceEx` çağrısı sıfırdan farklı `FREQUENCY` ile başarılı mı (sunum parametresi ile ekran modu Hz'i farklıyken / aynıyken) |

`fullscreen-refresh` kısa süreli tam ekran açar (masaüstü çözünürlüğüyle).

Derleme (`vcvars64.bat` sonrası, repo kökünden):

```bat
cl /nologo /EHsc /O2 /utf-8 tools\config-test\roundtrip.cpp client-src\src\Config\ConfigFile.cpp
cl /nologo /EHsc /O2 /utf-8 /Iclient-src\extern\include tools\config-test\fullscreen-refresh.cpp /link client-src\extern\library\DirectX\d3d9.lib user32.lib
```
