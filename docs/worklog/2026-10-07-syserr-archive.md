# syserr.log yeniden başlatmada korunuyor (roadmap T-1)

- **Tarih:** 2026-10-07
- **Tür:** özellik / karar
- **Alan:** server-src / runtime / tools
- **Durum:** Aktif
- **PR / commit:** [#21](https://github.com/heyomert/metin2-development/pull/21)

## Problem / hedef
`syserr.log` her açılışta sıfırlanarak açılıyordu (`libthecore/log.cpp`, `basic_file_sink_mt("syserr.log", true)`): çöküp ya da
durdurulup yeniden başlayan sürecin hata kaydı yeni açılışta siliniyordu. Somut kayıp: `db.core` (2026-10-05 19:52) çöküşünün
`syserr`'i bir sonraki açılışta gitti. AsyncSQL düzeltmesinin önce/sonra karşılaştırmasından önce kapanması gerekiyordu.

## Kök neden / kanıt
- Sıfırlama: `basic_file_sink_mt(..., truncate=true)`; spdlog `file_helper::open` dosyayı `wb` ile açıp kapatıyor.
- **Çökmeden hemen önceki satırlar da kayboluyordu** (VM denemesi, aynı logger ayarları, 20'şer tekrar): SIGSEGV satırdan 50 ms
  sonra → 0/20 diskte; `flush_on(err)` ile 20/20. `abort()` satırdan hemen sonra (`CHECKPOINT` yolu) → 0/20, `flush_on` ile de
  ~1/20 (satır hâlâ kuyrukta). FreeBSD `abort()` stdio tamponunu boşaltıyor (50 ms sonra abort → 20/20), SIGSEGV boşaltmıyor.
- spdlog async logger'da `flush_on` kontrolü **log thread'inde** (`async_logger::backend_sink_it_`); üretici sadece kuyruğa koyar
  (`async_logger::sink_it_` override'ı `logger::sink_it_`'teki flush kontrolünü atlar). Üreticiye ek iş yok — ölçümle doğrulandı.
- **libc++ tuzağı:** `fs::symlink_status(p, ec)` var olmayan yol için `not_found` döndürür **ve** `ec`'yi `ENOENT` yapar. İlk
  sürüm `ec`'yi hata sayıp her arşivlemeyi "ad bulunamadı" ile reddetti; fallback sayesinde hiçbir şey kaybolmadı (test yakaladı).
- **db "End of pid" hiç yazmıyor** (T-2 uçtan uca testinde görüldü): `db/Main.cpp:121` `log_destroy()`, `CNetPoller` yıkıcısındaki
  `thecore_destroy()`'dan önce; upstream. Roadmap teknik borç.

## Reddedilen yaklaşımlar
- **Sadece sona ekle (`truncate=false`):** önceki çalışmalarla sınırsız büyür, "`syserr.log` = bu çalışma" anlamı (dokümanlar,
  kontrol listesi, 1c karşılaştırması) bozulur.
- **spdlog `rotating_file_sink(rotate_on_open)`:** taşıma başarısız olursa dosyayı **sıfırlıyor** ve kurucuda istisna fırlatıyor
  (`rotating_file_sink-inl.h`) → kanıt kaybı ve sessiz açılış hatası (`start.py` çıktıyı atıyor).
- **`start.py`/rc script'inde taşıma:** kök neden binary'de; başka başlatma yollarını kapsamaz.
- **Arşivi syslog sink'i gibi kopyala + sil:** syslog sink'inde `copy_file` sonrası hata kontrolsüz `remove` var; burada hiçbir
  hata yolu silmez.
- **Başarılı arşivlemeyi `syserr`'e yazmak:** normal açılışın `syserr` içeriğini değiştirirdi; syslog'a yazılıyor.

## Çözüm
`libthecore/log.cpp` `archive_previous_syserr()`: sink açılmadan önce dolu `syserr.log` → `log/syserr_<son yazma>.log` (ad doluysa
`_N`; POSIX `rename` hedefin üzerine yazdığı için ad önceden kontrol edilir). Taşınamazsa, dosya bilgisi okunamazsa ya da beklenmedik
istisna olursa: sıfırlama yok, sona ekleme + tek `SYSERR_ARCHIVE` uyarısı (logger kurulduktan sonra doğrudan `g_syserr`'e; o anda
`_sys_err` satırı atardı). Saklama: en yeni 30, yalnız tam ad desenli normal dosyalar; yeni taşınan arşiv budanmaz (saat geri
alınırsa en eski görünebilir). `g_syserr->flush_on(err)`. Sözleşme: `docs/monitoring.md` → "Hata kaydı ve önceki çalışmalar".

## Doğrulama
- VM derlemesi: 0 hata, 372 uyarı (baseline), `log.cpp`'de 0.
- `tools/syserr-archive/run.sh cases` (gerçek `liblibthecore.a`, korumalı dizinler, gerçek hatalar): yok/boş dosya; arşiv adı
  son yazma zamanı, içerik birebir; `syserr.log` sadece bu çalışma; bildirim syslog'da, `syserr`'de değil; `log/` yok → oluşturuldu;
  ad çakışması → `_2`; `log` dosya / `log` değiştirilemez (`schg`) → eski içerik dosyanın başında birebir, sona ekleme, tek uyarı;
  32 → 30 budama, sayısal sonek sırası (`_2` < `_10`); `syslog_`/`metrics_`/`sql_`, 5 benzer ad, aynı adlı dizin ve symlink
  (ve hedefi) dokunulmadı; saat geri → yeni arşiv korundu; silinemeyen arşiv → diğerleri budandı, tek uyarı; ardışık 3 çalışma;
  SIGSEGV 20/20 satır diskte ve sonraki açılış arşivledi; `abort()`-hemen 1/20 (bilinen sınır). PASSED.
- `tools/syserr-archive/run.sh load` (200.000 satır, `flush_on` kapalı/açık dönüşümlü, VM'de canlı sunucu çalışırken): saat
  okumasız toplam süre satır başına medyan 19,0 µs (kapalı) / 18,9 µs (açık); kuyruk en çok 137/16.384 (`block` hiç devreye
  girmedi); boşaltma ≤1 ms; her satır diskte. Tek tük ms'lik tepeler iki modda da. Not: bu VM'de `steady_clock::now()` ~11,6 µs,
  satır başı ölçümün mutlak değerini şişirir; karşılaştırmayı etkilemez.
- `tools/sql-reliability/run.sh` aynı libthecore ile: koruma öz-testi 3/3 ret, 13 senaryo 10/10 aynı; S1–S11 davranış satırları
  (`stats` hariç) 1a baseline'ıyla birebir aynı (26/26). `sqlrt` her senaryoyu boş dizinde çalıştırdığı için arşivleme devreye
  girmiyor.
- **Henüz doğrulanmadı:** çalışan süreçlerde (deploy + yeniden başlatma, ayrı onay).

## Bir dahaki sefere tuzaklar
- libc++'ta `status`/`symlink_status` var olmayan yolda `ec` doldurur; önce `type() == not_found`'a bak.
- Bu VM'de saat okuması pahalı (~11,6 µs, ACPI-fast); satır başı zamanlama yapan ölçümler mutlak maliyeti şişirir.
- `syserr`'e satır başına ~19 µs oyun thread'i maliyeti (bu VM, 200 B satır) — istemcinin tetiklediği satırlar (A-14) bu yüzden
  disk kadar CPU açısından da önemli.
