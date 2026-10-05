# Zemin dokuları bulanıktı: DX9 geçişinde anizotropi ayarı kaybolmuş

- **Tarih:** 2026-10-05
- **Tür:** düzeltme
- **Alan:** client-src
- **Durum:** Aktif
- **PR / commit:** [#11](https://github.com/heyomert/metin2-development/pull/11)

## Problem / hedef
Aynı çözünürlükte (1366×768 tam ekran) başka bir files (AnadoluMt2) belirgin şekilde daha net görünüyordu;
bizde zemin dokuları birkaç metre ileriden itibaren bulanıklaşıyordu.

## Kök neden / kanıt
- Client zemin için anizotropik filtre istiyor (`StateManager::SetBestFiltering`, `GameLib/MapOutdoorRenderHTP.cpp:55-56`;
  `CPythonGraphic::SetGameRenderState`; su), ama `D3DSAMP_MAXANISOTROPY` hiçbir yerde ayarlanmıyordu.
  `tools/render-test/sampler-reset.cpp`: yeni D3D9Ex cihazında varsayılan `MAXANISOTROPY = 1`; anizotropi 1 doğrusal
  filtreyle aynı.
- Upstream'in ilk sürümü (`4be475f1`, `StateManager.cpp` `SetDevice`) cihaz desteğine göre en iyi filtreyi seçip
  `min(MaxAnisotropy, 4)`'ü 8 aşamaya yazıyordu (`D3DTSS_MAXANISOTROPY`). DX9 geçişinde (`e87b6fc6`, 2025-08-19)
  bu blok silindi (DX9'da ayar `D3DSAMP_MAXANISOTROPY` oldu); destek kontrolü de gitti, filtre koşulsuz
  `D3DTEXF_ANISOTROPIC` yapıldı.
- Aynı geçişte sampler durumları ayrı bir önbelleğe (`m_SamplerStates`) taşındı ama `ResetState()`'e eklenmedi;
  `CStateManager` `new` ile oluşturulduğu için (`EterLib/GrpDevice.cpp:527`) önbellek başlatılmamış bellekle
  başlıyordu: tesadüfen eşleşen bir değer cihaza hiç yazılmazdı.
- Anka2 aynı ayarı DX9 adıyla uyguluyor (`EterLib/StateManager.cpp:213-226`). AnadoluMt2'nin kaynağı yok;
  netlik farkının sebebinin bu olduğu ölçümle değil, kendi client'ımızdaki önce/sonra farkıyla destekleniyor.
- Bu makine: Intel UHD Graphics, `MaxAnisotropy = 16`, `MIN/MAGFANISOTROPIC` destekli (D3D9 caps).

## Reddedilen yaklaşımlar
- **16x ya da ayarlanabilir seviye:** Kullanıcı kararı: orijinal client'ın değeri 4x (az yük, belirgin kazanç).
- **Sadece `SetDevice`'ta yazmak (orijinaldeki gibi):** Cihaz sıfırlamasından sonra uygulanacağı garanti değil;
  `SetDefaultState` her sıfırlamadan sonra çağrılıyor (`GrpScreen.cpp` `RestoreDevice`, `GrpDevice.cpp`
  `ResizeBackBuffer` ve tarayıcı modu). Not: bu sistemde D3D9Ex `Reset` sampler durumlarını korudu
  (`sampler-reset.cpp`), yani "alt-tab sonrası POINT'e düşüyor" varsayımı burada geçerli değil.

## Çözüm
PR #11. `EterLib/StateManager`: `SetDevice` cihaz desteğine göre en iyi min/mag filtreyi seçer ve anizotropiyi
`clamp(MaxAnisotropy, 1, 4)` olarak hesaplar; `SetDefaultState` 8 aşamaya `D3DSAMP_MAXANISOTROPY` yazar;
`ResetState` sampler önbelleğini de sıfırlar. `CPythonGraphic::SetGameRenderState` ve su çizimi destek kontrolü
olmadan anizotropik istemek yerine aynı seçimi kullanır (`SetBestFiltering`, `GetBestMin/MagFilter`).

## Doğrulama
- Derleme: `/m:1` → 0 hata, değişen dosyalarda uyarı yok.
- Oyun içi, 1360×768 tam ekran, aynı yer (Pyungmoo), kayıpsız PNG (`CopyFromScreen`):
  `tools/render-test/sharpness.py` ortalama gradyan — uzak zemin 3,29 → 9,69 (+%195), orta 7,46 → 26,89 (+%260),
  yakın taşlar 21,23 → 29,53 (+%39). Kamera açısı iki karede biraz farklı; fark gözle de belirgin (uzak toprak/çim
  dokusu seçilebiliyor). FPS 60/61 (oyunun sabit sınırı). `syserr.txt` yalnızca önceden var olan `invalid idx 0`.
- Tam ekranda alt-tab sonrası (kullanıcı): zemin net kalıyor.
- Etki alanı (çizim sırası `PythonApplication.cpp:178-214`, `MapOutdoorRender.cpp` `OnRender`): bütün haritalar tek
  `CMapOutdoor` yolunu kullanıyor (`MapManager.cpp:109`). Zemin, binalar/objeler, su ve sonradan çizilen
  karakter/mob/yerdeki item'lar en iyi filtreyle; ağaçlar SpeedTree'nin sabit `LINEAR`'ıyla
  (`SpeedTreeForestDirectX.cpp:193-195`, değişmedi); arayüz/ikonlar ekran hizalı, anizotropiden etkilenmez.
- Diğer çözünürlükler ölçülmedi (mekanizma çözünürlükten bağımsız).

## Bir dahaki sefere tuzaklar
- Tam ekran oyuna alt-tab yapınca pencere küçülüyor; oyun yönetici yetkisiyle çalıştığı için normal yetkili bir
  program onu öne getiremez. Ekran görüntüsünü oyun öne gelince otomatik alan bir bekleyiciyle al.
- Oyunun kendi ekran görüntüsü JPEG; netlik karşılaştırmasında kullanma.
