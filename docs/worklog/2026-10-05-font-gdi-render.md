# Yazı çizimi: FreeType LCD yerine orijinal GDI + 1-bit eşik (Anka2 görünümü)

- **Tarih:** 2026-10-05
- **Tür:** düzeltme / karar
- **Alan:** client-src
- **Durum:** Aktif
- **PR / commit:** [#9](https://github.com/heyomert/metin2-development/pull/9)

## Problem / hedef
Oyundaki yazılar soluk, kenarları bulanık; kontur katı siyah değil. Hedef: oyunun her yerinde Anka2'deki gibi
net yazı ve katı siyah kontur.

## Kök neden / kanıt
- Upstream 3–4 Şubat 2026'da yazı çizimini Windows GDI'dan FreeType'a taşıdı (`ce5ef584`), kerning (`ba514d2e`) ve
  LCD alt-piksel yumuşatma (`87cfa154`) ekledi. Ondan önceki upstream kodu Anka2'deki kodla aynı algoritma:
  GDI `ANTIALIASED_QUALITY` + `(px & 0xff) ? 0xffff : 0` eşiği + `A4R4G4B4` doku.
- Görüntü farkının sebebi **eşikleme / kenar yumuşatma**. Kontur geometrisi iki tarafta aynı (dört yöne 1 px).
  `test-files` raporundaki "2. geçiş atlandığı için kontur lekeli" açıklaması yanlış: siyah kontur için 1. geçiş
  matematiksel olarak doğru karıştırma.
- **Boyut farkı:** GDI'da pozitif `lfHeight` hücre yüksekliği (ascent+descent), FreeType'ta `FT_Set_Pixel_Sizes`
  em boyutu. Tahoma için `12` → GDI em ≈ 9,94 px, FreeType 12 px: yazılar ~%20 büyük çiziliyordu. UI ölçüleri
  orijinal (GDI) metriklerle tasarlandı.
- Ölçüm (`tools/font-compare/glyph-compare.cpp`, 106 karakter: ASCII + Türkçe):

  | Font | En iyi FreeType ayarı | Fark |
  |---|---|---|
  | Tahoma 12 / 14 / 18 | mono hinting, em eşlenmiş | %0,1 / %0 / %0,5 |
  | Tahoma 12 italik | mono | %1,2 |
  | Tahoma 9 (`UI_DEF_FONT_SMALL`) | light hinting + eşik | **%22,9** |
  | Arial 12 (Anka2 `TipBoard`) | mono | **%117** |
  | Mevcut kod (LCD, em = boyut), Tahoma 12 | — | %258 |

- Kullanım: `UI_DEF_FONT` Tahoma 12 neredeyse her yerde, `_LARGE` 14 başlıklar, `_SMALL` 9 sadece sürüklenen
  item adedi (`client/assets/root/mousemodule.py:139`). Duyuru panoları `CTextBar`: Anka2'de Arial
  (`GetFontFaceFromCodePage`), `TipBoard` 12 normal, `BigBoard` 18 kalın.
- `tools/font-compare/cell-overhang.cpp`: GDI bazı harfleri hücrenin soluna taşırıyor (negatif A genişliği):
  Tahoma 12'de `ì î ï Ħ ħ ĩ ī ĭ ļ ł`, Tahoma 9/14'te `j`, italikte `ş`, Arial 18 kalında `A`. Orijinal kod bu
  pikselleri eşiklemeden önceki harfin hücresine yazıyordu → yan harfte soluk renkli kalıntı.

## Reddedilen yaklaşımlar
- **FreeType'ı koruyup mono/eşik modu:** Tahoma 9 ve Arial 12'de Anka2'yi tutturamıyor (yukarıdaki tablo);
  "her yerde aynı" hedefini karşılamıyor.
- **`test-files` kopyasındaki değişikliği almak:** sadece 2 dosyaya dokunuyordu (`TextBar` FreeType'ta kaldı),
  ilgisiz 144 FPS değişikliğini içeriyordu, taşan harf hatasını taşıyordu.
- **Kerning'i GDI ile (`GetKerningPairs`) korumak:** Anka2/orijinal client kerning yapmıyor; görüntü farklı olur.

## Çözüm
Upstream'in FreeType öncesi GDI yolu temel alındı, sonradan gelen düzeltmeler korundu (PR #9):
- `GrpDIB`: kendi bellek DC'si olan 32-bit DIB section.
- `GrpFontTexture`: GDI ile harf çizimi, sayfa yüklenirken 1-bit eşik, `A4R4G4B4`. Taşan harfler için hücreye sol
  pay + `bearingX` (taşmayan harflerde görüntü orijinalle aynı). Cihaz sıfırlanınca harfler yeniden çizilir;
  kayıtlar yerinde güncellenir (yazı nesnelerinin tuttuğu işaretçiler geçerli kalır).
- `GrpTextInstance`: kerning kaldırıldı; iki geçişli LCD çizimi yerine tek geçiş (önce kontur, sonra yazı).
- `TextBar` + `BlockTexture`: GDI, Arial; dokunulan piksel tam opak. `BigBoard` kalın (`PythonGraphicModule.cpp`).
  Upstream'in eski `TextBar`'ındaki font sızıntısı (`DeleteObject` yoktu) düzeltildi.
- `FontManager` silindi, FreeType build'den çıkarıldı (`vendor/freetype-2.13.3` klasörü diskte duruyor).
- **Upstream'den bilinçli ayrışma (A-8):** upstream'in `EterLib` font dosyalarındaki değişiklikleri alırken bu
  kararı koru; doğrudan merge çakışır.

## Doğrulama
- `tools/font-compare/new-vs-original.cpp` (yeni çizim = orijinal/Anka2 çizimi mi?): Tahoma 9/12/14/12i, Arial 12,
  Arial 18 kalın × 916 harf (Latin, Latin-1, Latin Ext-A/B, Yunanca, Kiril). Orijinalin ekrandaki piksellerinden
  eksik **0**, fazla **0** (tek fark: orijinalde komşu hücreye düşen sol taşma pikselleri artık doğru yerde),
  yeni hücrenin dışına çizilen piksel **0**, `OPAQUE`/`TRANSPARENT` arka plan farkı **0**.
- Derleme: `cmake --build build --config Release -- /m:1` → 0 hata; değişen dosyalarda uyarı yok.
- Client açıldı, karakter seçim ekranı çizildi: Türkçe karakterler (ğ ı ş Ç ü) doğru, yazılar 1-bit, kontur katı
  siyah. `syserr.txt` tek satır: `invalid idx 0` — lonca amblemi (`UserInterface/MarkManager.cpp:282`), font ile
  ilgisiz.
- Oyun içi (kullanıcı, 2026-10-05): yazılar oyunun her yerinde çalışıyor. Item tooltip renkleri ekran
  görüntüsünden piksel olarak ölçüldü: başlık `241,230,192` = `TITLE_COLOR`, açıklama `193,193,193` = `NORMAL_COLOR`
  (`client/assets/root/uitooltip.py:86-88`); renkler değişmedi. Bonussuz item'ların krem başlığı Anka2 ile aynı kural
  (`uitooltip.py:905-913`).
- Çalışan client'ta GDI nesnesi 66 (tepe 68), sınır 10.000 (`GetGuiResources`, karakter seçim ekranı).
- Çözünürlük / tam ekran / alt-tab (kullanıcı, 2026-10-05, kaynaktan derlenen yeni `config.exe` ile): 1920×1080 ve
  1366×768 tam ekranda yazılar net ve konturlu; tam ekranda alt-tab sonrası (cihaz sıfırlama yolu) yazılar bozulmadı.
  Eski `config.exe` ayarları client'ın okumadığı dosyaya yazdığı için bu test önce yapılamamıştı (roadmap → Teknik borç).
- Görev listesinde uzun adların yan sütuna taşması bu değişiklikten önce de var (sabit 100 px sütun,
  `client/assets/root/interfacemodule.py:1375`); font ile ilgisiz.


## Bir dahaki sefere tuzaklar
- Client'ın `fonts/` klasöründen font yükleme artık yok: GDI sadece Windows'ta kurulu fontları görür
  (Tahoma, Arial her Windows'ta var).
- VS derlemesi `/m` ile bu makinede bellek yetmiyor (C1060, "disk belleği dosyası çok küçük"); `/m:1` ile derle.
