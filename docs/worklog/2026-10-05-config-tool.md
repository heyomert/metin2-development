# config.exe kaynaktan yeniden yazıldı; tam ekranda frekans seçimi MSAA'yı kapatıyordu

- **Tarih:** 2026-10-05
- **Tür:** düzeltme / karar
- **Alan:** client-src / client
- **Durum:** Aktif
- **PR / commit:** <PR linki>

## Problem / hedef
`config.exe`'de seçilen çözünürlük ve tam ekran oyunda hiç uygulanmıyordu. Hedef: ayarların `config.exe`
üzerinden doğru ve kalıcı şekilde yönetilmesi.

## Kök neden / kanıt
- Oyun ayarları `config/metin2.cfg`'den okur/yazar (`client-src/src/UserInterface/PythonSystem.cpp:401`, `:509`).
  Upstream bu dosyayı `config/` altına `729a6624` (2025-08-19) ile taşıdı.
- Eski `client/config.exe` kaynaksız hazır bir MFC programı ("Config MFC"); `client-src`'de onu üreten hedef yok,
  upstream'in diğer repolarında ve Anka2'de kaynağı yok. Exe içindeki tek dosya yolu `metin2.cfg` (client kökü).
  Kullanıcının makinesinde `client/metin2.cfg` 1360×768 tam ekran, `client/config/metin2.cfg` 1024×768 pencereydi.
- Biçim de uyumsuz: ses seviyelerini `2`, `5` gibi tam sayı yazıyor; oyun 0–1 arası ondalık bekliyor (`%.3f`,
  `PythonSystem.cpp:428-430`). Oyunun `OBJECT_CULLING`, `FOG_LEVEL`, `SAVE_ID` vb. satırlarını bilmiyor.
- Bu sırada oyunun kendisinde iki sorun bulundu (eski exe'de ve Anka2'de de var):
  1. **Frekans tam ekranı bozuyor.** `CGraphicDevice::Create` sunum parametrelerine her zaman
     `FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT` (0) yazıyor, `D3DDISPLAYMODEEX::RefreshRate`'e ise
     `FREQUENCY` (`EterLib/GrpDevice.cpp:396`, `:436`). `tools/config-test/fullscreen-refresh.cpp` ile ölçüldü
     (masaüstü 1920×1080 @ 144 Hz): 0/0 → `S_OK`, 0/144 → `0x8876086C` (`D3DERR_INVALIDCALL`), 144/144 → `S_OK`.
     Başarısızlıkta oyun Hz'i 0'a çekip **MSAA'yı kapatarak** tekrar deniyor (`GrpDevice.cpp:470-475`); Release'de
     `Tracen` boş olduğu için (`EterBase/Debug.cpp:413-425`) hiçbir iz kalmıyor.
  2. **Gamma hiç uygulanmıyor.** Sadece `CPythonSystem::ApplyConfig` uyguluyor; onu Python arayüzü hiç çağırmıyor
     (`client/assets/root`'ta 0 eşleşme), açılışta da çağrılmıyor. Anka2'de de aynı. "Gamma" aslında parlaklık
     çarpanı (`EterPythonLib/PythonGraphic.cpp:127-157`), sadece tam ekranda etkili, 1,2 ve 1,4'te açık renkleri
     kırpıyor. **Karar: bilerek bırakıldı** (aşağıda).

## Reddedilen yaklaşımlar
- **Eski exe'yi yamalamak** (içindeki yolu değiştirmek): hack; ses biçimi ve eksik anahtarlar sorunu kalır.
- **Frekans hatasını bırakmak:** Hz seçen oyuncunun MSAA'sı sessizce kapanır.
- **Gamma'yı açılışta uygulamak** (önce uygulandı, sonra geri alındı): Varsayılan `3` = 1,2 herkesi %20 parlatır,
  repodaki `config/metin2.cfg` (`GAMMA 1` = 0,7) %30 karartırdı; bunu önlemek için varsayılanı nötr `2` yapmak
  gerekti. Kullanıcı kararı: gerek yok — bugüne kadar hiçbir files'ta çalışmadı, parlaklık monitör/Windows/ekran
  kartı panelinden (pencere modunda da) ayarlanabiliyor, ayar kaba (kırpıyor) ve sadece tam ekranda. Bu laptop'ta
  oyun tam ekrandayken GDI `GetDeviceGammaRamp` nötr rampa okudu; tam ekran gamma'nın ekrana ulaşıp ulaşmadığı
  kanıtlanamadı (oyuncu o an alt-tab'da olabilirdi). Gamma oyunda olduğu gibi kaldı, `config.exe`'den kaldırıldı;
  `GAMMA` satırı dosyada korunuyor.

## Çözüm
PR'da. Özet:
- `client-src/src/Config`: Win32 diyalog (MFC yok), çıktı `config.exe`. `config/metin2.cfg`'yi oyunun
  biçimiyle yazar; yönetmediği satırları korur; oyun ilk boş satırda okumayı bıraktığı için boş satırları atar;
  önce geçici dosyaya yazıp sonra değiştirir. Çözünürlük listesi `CPythonSystem::GetDisplaySettings` ile aynı
  kurallarla (varsayılan adaptör, masaüstü biçimi, ≥800×600, 16/32 bpp). Değerler oyunun aralıklarına sıkıştırılır
  (eski exe'nin `MUSIC_VOLUME 2`'si 1,000 olur). Normal yetkiyle çalışır.
- `GrpDevice.cpp`: `FullScreen_RefreshRateInHz = iReflashRate` (ekran modundaki değerle aynı).
- Repodaki `client/config/metin2.cfg`: `FREQUENCY 50 → 0` (50 Hz artık gerçekten istenecekti; monitör
  desteklemezse MSAA'sız geri dönüş yoluna düşerdi).

## Doğrulama
- `tools/config-test/roundtrip.cpp`: hepsi geçti — oyunun `LoadConfig` döngüsünün kopyasıyla okunan değerler doğru,
  yönetilmeyen anahtarlar korunuyor, eski exe'nin dosyası düzeliyor, ortadaki boş satır sorunu gideriliyor,
  dosya yokken boş ayar, ikinci kayıt sabit.
- Pencere gerçek ayar dosyasının kopyasıyla açıldı; bütün kontroller dosyadaki değerleri gösterdi.
- Kullanıcı (yeni `config.exe` + font exe): 1920×1080 ve 1366×768 tam ekran uygulandı, alt-tab sorunsuz.
- Frekans, düzeltmeden sonra (`tools/config-test/fullscreen-refresh.cpp 60`, oyunun 4x MSAA isteğiyle):
  0/0 → `S_OK`; eski kod 0/60 → `0x8876086C`; yeni kod 60/60 → `S_OK`. Bu laptop'ta (ekranı Intel UHD sürüyor,
  RTX 3050 Laptop da var) cihaz 60 Hz ile oluşsa da ekran 144 Hz'de kalıyor (D3D `GetDisplayModeEx`, Windows
  `ENUM_CURRENT_SETTINGS` ve WMI hepsi 144). Oyunda `FREQUENCY 144` ve `60` ile de WMI 143 okudu. Yani burada Hz
  uygulanmıyor ama MSAA artık kapanmıyor; Hz'i doğrudan bağlı monitörlü bir makinede ölçmek **Unverified**.
  Ters yönde de aynı: masaüstü 60 Hz iken 144 istendiğinde eski kod `0x8876086C`, yeni kod `S_OK`, ekran 60 Hz'de kaldı.
- FPS 61 sabit: oyunun kendi sınırı, Hz'den bağımsız (`CTimer::UseCustomTime` → kare başına 16/17 ms,
  `EterBase/Timer.cpp:95-105`; `PythonApplication.cpp:543-548` `Sleep`).

## Bir dahaki sefere tuzaklar
- `Metin2.exe` `requireAdministrator` ile derleniyor (`UserInterface/CMakeLists.txt:4`): normal yetkili bir
  süreç onu sonlandıramaz ("Erişim engellendi").
- `client/config/metin2.cfg` repoda takip ediliyor ama oyun ve `config.exe` onu yerel ayar olarak değiştiriyor;
  yerel değişikliği commit'leme, sadece bilerek değiştirilen varsayılanları commit'le.
- Release build'de `Tracen`/`Tracenf` boş: `CreateDevice` hataları log'a düşmez.
- Branch değiştirince `client/Metin2.exe` (LFS) o branch'in exe'sine döner; test ederken hash'i kontrol et.
