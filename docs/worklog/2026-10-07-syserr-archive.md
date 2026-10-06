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
- `tools/syserr-archive/run.sh load` (200.000 satır, `flush_on` kapalı/açık dönüşümlü, 5'er koşu, VM'de canlı sunucu
  çalışırken), üç ölçüm modu ayrı:
  - `bare` (döngüde saat/kuyruk okuması yok, tam hız): satır başına medyan **19,8 µs (kapalı) / 20,2 µs (açık)**.
  - `sampled` (kuyruk her 256 satırda, saat her 1024 satırda; `bare`'e yakın: medyan 19,1 / 19,7 µs): örneklenen kuyruk tepesi
    medyan 76 / 160, en çok **86 / 352**; gerçek tepe ≤ örnek + 256 (+4 periyodik flush mesajı) → en kötü **≤ 612 / 16.384**:
    kuyruk hiç dolmadı, `block` üreticiyi hiç bekletmedi. `flush_on` açıkken log thread'i biraz yavaşlıyor (kuyruk tepesi
    büyüyor) ama yetişiyor: üretim bitiminde kuyrukta ≤7 mesaj, boşaltma ≤1 ms. 1024'lük parti başına satır süresi p99 medyanı
    27,9 / 26,8 µs, en kötü parti 37,5 / 59,3 µs.
  - `per-line` (satır başına iki saat okuması; bu VM'de `steady_clock::now()` ~11,6 µs → üretici ~3 kat yavaş, satır başı
    toplam ~56 µs): p50 ~26–30 µs, >10 ms 0. Bu moddaki kuyruk tepeleri (≤164) yavaş üreticiye ait; `block` kanıtı için
    `sampled` kullanılır (önceki "137/16.384" rakamı bu yavaş moddan geliyordu, düzeltildi).
  - Her koşuda 200.000 satırın hepsi diskte.
- `tools/sql-reliability/run.sh` aynı libthecore ile: koruma öz-testi 3/3 ret, 13 senaryo 10/10 aynı; S1–S11 davranış satırları
  (`stats` hariç) 1a baseline'ıyla birebir aynı (26/26). `sqlrt` her senaryoyu boş dizinde çalıştırdığı için arşivleme devreye
  girmiyor.
- **Uçtan uca, test VM (2026-10-07, `8fbec589` = PR #21 merge, `archive` + `--policy test-vm`):**
  - A: stop (5 game "End of pid", db `reason=final`) → consistent yedek `result=ok` → kurulum (`.prev` = T-2 çifti + `BUILD`) →
    start → 6 süreç; kimlik zinciri (`version`, `BUILD:`, sağlık/SQL `build=`, `BUILD`, `deploy.log`, SHA-256) aynı; **6/6 arşiv
    durdurma sonrası kopyayla SHA-256 birebir**, adlar son yazma zamanıyla aynı, syslog'da bildirim, `syserr`'de uyarı yok,
    normalize `syserr` önceki açılışla aynı; giriş (core1 seçim → core3 map 41), SQL hata/retry/takılma 0.
  - B: `channel1_core2` (`users_local=0`, istemci bağlantısı 0; PID üç kaynaktan doğrulandı) `kill -9` → diğer 5 süreç ayakta;
    beklenen izler: db `FDWATCH: peer null` (1 satır), diğer çekirdeklerde P2P soketi kapandı; zincirleme etki yok. stop (`stop.py`
    ölü PID'i atladı) → start → **6/6 yeni arşiv birebir**, A'nın arşivleri değişmedi; core2 arşivinde "End of pid" ve SIGTERM
    yok, 4 game'de var, db'de beklendiği gibi yok; son giriş, SQL hata 0.
  - Gözlemler: (1) ölen çekirdeğin arşiv adı ölüm değil son yazma zamanı (`00-27-14`, açılış satırları) → `docs/monitoring.md`;
    (2) `STATE_MOVE_ZERO_DURATION` uyarıları oyuncu hareketinden, upstream kod (`game/char_state.cpp:770`), T-1 ile ilgisiz;
    (3) db `workers_down=8` sadece eski süreçlerin kapanış `sum` satırlarında (beklenen).
  - **Boş PID (operasyonel bulgu, T-1 blocker değil):** ilk denemede PID çıkarımı boş döndü (çok satırlı `pids.json`'u `grep`
    ayrıştıramadı) ve ad-hoc komut boş PID'de fail-closed durmuyordu; `kill` kullanım hatası verdi, **hiçbir sinyal gönderilmedi,
    hiçbir süreç etkilenmedi** (6 süreç aynı PID'lerle çalışmaya devam etti). Asıl testte PID üç bağımsız kaynaktan doğrulandı:
    JSON olarak `pids.json`, çekirdeğin `pid` dosyası, `procstat` (binary yolu + çalışma dizini).
- **CRLF (ölçüldü):** `git archive` sistem `core.autocrlf=true` yüzünden metin üyelerini CRLF'ye çeviriyordu (`log.cpp`: 304 CR);
  `-c core.autocrlf=false` arşivinde üyeler blob'larla aynı (`SOURCE_COMMIT` ve 3 LFS resmi hariç, beklenen) →
  `docs/build-and-run.md` tuzak 3.

## Bir dahaki sefere tuzaklar
- libc++'ta `status`/`symlink_status` var olmayan yolda `ec` doldurur; önce `type() == not_found`'a bak.
- Bu VM'de saat okuması pahalı (~11,6 µs, ACPI-fast); satır başı zamanlama yapan ölçümler mutlak maliyeti şişirir.
- Süreç öldüren/sinyal gönderen bir komut tekrar kullanılacak bir script'e girerse zorunlu koruma: PID boş olamaz, yalnız sayısal,
  beklenen süreç adı/binary/çalışma dizini ile eşleşmeli; eşleşmezse sinyal gönderilmeden fail-closed dur. (Bu testteki komut
  ad-hoc'tu; ürün kodu yazılmadı.)
- `syserr`'e satır başına ~19 µs oyun thread'i maliyeti (bu VM, 200 B satır) — istemcinin tetiklediği satırlar (A-14) bu yüzden
  disk kadar CPU açısından da önemli.
