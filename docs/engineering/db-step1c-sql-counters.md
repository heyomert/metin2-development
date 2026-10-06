# DB adım 1c: AsyncSQL kuyruk, bekleme ve hata sayaçları — etki analizi

**Durum: revizyon 2 onaylandı ve uygulandı (2026-10-06, branch `feat/sql-counters`).** Uygulamada değişenler ve
sonuçlar: bölüm 10. Üst plan: `docs/engineering/db-standard.md`
(bölüm 7), `docs/engineering/db-step1-measurement.md` (1c ilk taslağı; bu belge onu **daraltır** ve gerçek koda göre düzeltir).
Kaynak yollar `server-src/src/` altındadır. Etiketler: **Kanıtlı**, **Bilinmiyor**, **Öneri**.

**Temel şart:** gözlem gözleneni değiştirmez. Kuyruk sırası, retry, log satırları, kapanış ve sorgular aynı kalır; ölçüm
bozulursa sadece ölçüm eksik kalır. **Ana kabul kriteri:** 1a'nın 12 senaryosu önce/sonra birebir aynı sonucu verir.
1c hata düzeltmez; onları doğru adla ve düşük maliyetle görünür yapar.

## 0. Kapsam (dar)
| Var | Yok (bilerek) |
|---|---|
| Kuyruk: ana / kopya / sonuç kuyruğu, en eski bekleyenin yaşı, takılı baş sorgunun süresi, worker durumu | Retry/kuyruk/kapanış düzeltmesi (adım 2) |
| Hata: hatasız/hatalı biten, tekrar denemesi, hata kodu kovaları (1205 ile 1213 ayrı), gözlenen yeniden bağlanma | İfade sınıfı, SQL metni, tablo adı |
| Bekleme: sorgu süresi — **mevcut** saat okumalarından | Yeni sorgu, `PROCESS`, `performance_schema` |
| db: SAVE türü sorguların sonucu (QID bazında) | db önbellek boyutları, flush sayıları, db döngü süresi |
| db: kapanışta çalıştırılmadan kalan sorgu sayısı | game kapanış sırasını telemetri için değiştirmek |

## 1. Mevcut durum (Kanıtlı)
### 1.1 Bağlantılar ve hedefleri
| Sahip | Rol | Hedef (config) | Kanıt |
|---|---|---|---|
| game `DBManager` | `main` (thread: Async+Return), `direct` | auth çekirdeği `account_sql`, diğerleri `player_sql` | `game/config.cpp:902-906`, `game/db.cpp:33,36` |
| game `AccountDB` | `main`, `direct` | **`common_sql`** (adı yanıltıcı) | `game/config.cpp:854,900`, `config.cpp:432-454` (`db_*[2]` = `common_sql`) |
| game `LogManager` | `main` | `log_sql` | `game/config.cpp:919`, `game/log.cpp:23` |
| db `CDBManager` slot × rol | `main` (ReturnQuery), `async`, `direct` | `player`, `account`, `common`, `hotbackup` | `db/DBManager.h:81-83`, `db/DBManager.cpp:88-110`, `db/Main.cpp:253-335` |
→ Satırlarda üç ayrı makine-okunur alan: `owner` (sahip nesne), `target` (hangi config DB'si), `role`.

### 1.2 SAVE sonuçları ve gerçek gözlem noktası
- db'deki **bütün** ReturnQuery'ler varsayılan slot `SQL_PLAYER`'ın `main` bağlantısından geçer (`db/DBManager.h:43`); bunlar
  yükleme sorgularını da (`QID_PLAYER/ITEM/QUEST/AFFECT`, `QID_LOGIN`, …) içerir. → `player/main` bağlantısının hata sayısı
  **SAVE hatası değildir**; SAVE'ler QID bazında ayrıca sayılmalı.
- SAVE gönderen yerler: cache flush `QID_PLAYER_SAVE` (`db/Cache.cpp:171`), `QID_ITEM_SAVE` (`:143`), `QID_ITEM_DESTROY`
  (`:61`), ItemAward `QID_ITEM_AWARD_TAKEN` (`db/ItemAwardManager.cpp:118`) — hepsi **`dwIdent=0`**; peer'li olanlar
  `QID_QUEST_SAVE` (`db/ClientManager.cpp:441`), `QID_SAFEBOX_SAVE` (`:960`), `QID_ITEM_SAVE` (`:1287`), `QID_ITEM_DESTROY` (`:1548`).
- `AnalyzeQueryResult` peer'i `GetPeer(qi->dwIdent)` ile arar (`db/ClientManager.cpp:2487`); handle'lar 1'den başlar
  (`db/Peer.cpp:29-30`) → `GetPeer(0)` her zaman NULL → ilk switch'ten sonra `if (!peer) return` (`:2511-2515`).
  **Sonuç:** sayacı SAVE `case`'lerine (`:2556-2562`) koymak cache flush kaynaklı oyuncu/item kayıtlarının **hepsini**
  kaçırırdı. Doğru gözlem noktası: `AnalyzeQueryResult`'ın girişi, ilk `switch`'ten önce — tek çağıran
  `MainLoop`'taki sonuç döngüsü (`:182-186`), bütün sonuçlar oradan geçer; sayma davranışı değiştirmez.
- **Kapanışta:** `MainLoop` bittikten sonra bütün cache'ler flush edilir (`:195-235`), ama sonuçlar **hiç** `PopResult`
  edilmez (döngü bitti). Bu SAVE'lerin sonucu QID bazında görülemez; sadece bağlantı düzeyinde (`player/main` hata
  sayısı, kapanış satırı) görülür.
- **Anlam (Kanıtlı kod yolu):** `Flush()` sorguyu kuyruğa koyar koymaz kaydı temiz işaretler (`common/cache.h:48-56`);
  SAVE'ler tam satır yazar (`REPLACE INTO item…`, `db/Cache.cpp:140`; oyuncu: `CreatePlayerSaveQuery`). Hatalı bir SAVE =
  **o yazma uygulanmadı**. Aynı satırın sonraki başarılı bir SAVE'i onu düzeltir; düzeltmeden önbellekten düşerse ya da
  süreç kapanırsa veritabanı eski kalır. **Kalıcı kayıp sayaçla kanıtlanamaz**; metrik adı bunu iddia etmez.

### 1.3 AsyncSQL içi
- Üretici `PushQuery` kilit altında ana kuyruğa ekler (`libsql/AsyncSQL.cpp:234-241`); worker ana kuyruğun tamamını kopya
  kuyruğuna taşır (`:274-288`); kopya kuyruğuna sadece worker dokunur, kilitsiz (`:265-297`, `:392-470`).
- `CountQuery()` sadece ana kuyruğu sayar (`:314-318`); `CountQueryFinished()` başarısızları da sayar (`:469`, 1a S7); db bu
  sayaçları **her saniye sıfırlar** (`db/ClientManager.cpp:2675`, `db/DBManager.h:62-78`). → Yeni sayaçlar ayrı ve monoton;
  mevcutlara dokunulmaz.
- Worker her sorguda zaten `steady_clock` okuyor (`cProfiler::Start/Stop`, `:401,444`) → süre ek saat okumadan gelir.
- Yeniden bağlanma **sorgudan önce thread id karşılaştırmasıyla fark ediliyor** (`:404-410`, `DirectQuery` `:153-159`);
  bağlantının koptuğu/yenilendiği an değil. 1a S8: Connector/C sessizce yeniden bağlanıp gönderiyor, hata görünmüyor.
- Kapanış: worker ana kuyruğu işler, retry listesindeki hatalarda `continue` ile atlar (`:498-517`); kopya kuyruğunu hiç
  işlemez (1a S9). db sırası: `MainLoop` → cache flush → `DBManager.Quit()` → bekleme → `log_destroy()` (`db/Main.cpp:90-111`).
  game'de `DBManager` yıkıcısı `log_destroy()`'dan (`game/main.cpp:423`) sonra çalışır; `sys_err` o anda sessizce bir şey
  yapmaz (`libthecore/log.cpp:76-80`). → game'de kapanış sayımı yapılamaz; sıra telemetri için **değiştirilmez**.

### 1.4 Yazıcı ve izolasyon
- game sağlık kaydı kendi `spdlog::details::thread_pool`'unu (kuyruk 64, 1 worker, sinyaller maskeli) ve `discard_new`'u
  kullanıyor (`game/server_metrics.cpp:75-102`); `metrics_dropped` alanı **o havuzun** `discard_counter()`'ı (`:289`).
  → SQL satırları aynı havuza girerse mevcut `metrics_dropped`'ın anlamı değişir. **Ayrı havuz şart.**
- `common` INTERFACE kütüphanesi; game, db, libsql ona bağlı (`common/CMakeLists.txt`, `db/CMakeLists.txt`,
  `libsql/CMakeLists.txt`). db'nin spdlog başlıklarını görmesi `libthecore` üzerinden — **derlemede doğrulanacak**.

## 2. Mimari
### 2.1 Sayaçlar `libsql` içinde
Durum `CAsyncSQL`'in içinde; dışarıdan okumak veri yarışı. Eklenecekler (hepsi monoton, süreç başından beri):
- `std::atomic<uint64_t>` relaxed: `pushed`, `taken`, `ok`, `err`, `retry`, `reconnect_seen`, `result_pushed`,
  `result_popped`, hata kovaları, `exec_n`, `exec_sum_us`; tek yazıcılı `exec_max_us` (okuyan sıfırlamaz; bkz. 2.3).
- Anlık: `oldest_enqueue_ms` (bitmemiş en eskinin kuyruğa giriş anı), `stuck_since_ms`, `worker_running`,
  `unexecuted_at_quit` (sadece `Quit()` join'inden sonra yazılır).
- `SQLMsg`'e 8 baytlık `enqueue_ms` (zaten yapılan `make_unique` içinde; **ek ayırma yok**).
- `SQLStats GetStats() const` → düz yapı. **libsql metin üretmez, I/O yapmaz, kilit almaz.**
- Ucuz saat: FreeBSD `CLOCK_MONOTONIC_FAST`, Linux `CLOCK_MONOTONIC_COARSE`, diğer `steady_clock` (derleme zamanı).
- Kapanış döngüsünde biten/atlananlar da `ok`/`err`'e girer (çalıştırıldılar); kopya kuyruğunda kalanlar
  `unexecuted_at_quit`.

### 2.2 db SAVE sonuçları
`CClientManager::AnalyzeQueryResult` girişinde, ilk `switch`'ten önce: `qi->iType` SAVE türlerinden biriyse
`msg->uiSQLErrno == 0 ? ok : err` sayılır (db ana thread'i, düz sayaç). Türler: `QID_PLAYER_SAVE`, `QID_ITEM_SAVE`,
`QID_ITEM_DESTROY`, `QID_QUEST_SAVE`, `QID_SAFEBOX_SAVE`, `QID_ITEM_AWARD_TAKEN`. Başka hiçbir şey değişmez.

### 2.3 Ölçme ile yazmayı ayırma
- **Ölçüm** her 10 sn (game: sağlık penceresiyle aynı an, game thread; db: mevcut 10 sn bloğu `db/ClientManager.cpp:2711`,
  db ana thread): bütün bağlantıların `GetStats()`'ı okunur, önceki pencereyle karşılaştırılır.
- **Yazma:**
  - `kind=sum` — süreç başına **her 10 sn bir satır**: bütün bağlantıların penceredeki Δ'ları ve anlık en kötü değerler.
    Sürekli zaman serisi (lag ile ilişkilendirme), db'de SAVE sonuçları.
  - `kind=conn` — bağlantı başına; **sadece** o pencerede anormallik (`err`/`retry`/`reconnect_seen` Δ>0, `stuck_ms>0`,
    `oldest_ms ≥ 1000`, `worker=0`) varsa ve **her bağlantı için 5 dk'da bir** (A/B için bağlantı bazında ortalama; yokluk
    "bilinmiyor" ile karışmasın). Değerler **kümülatif toplam** (`*_total`): satır atlansa ya da düşse bile iki satır
    arası fark doğru kalır.
  - Hacim tahmini (doğrulanacak): `sum` ~300 B × 8.640/gün × 6 süreç ≈ 15 MB/gün; `conn` 5 dk'lık satırlar ~37 bağlantı ≈
    3 MB/gün + anormallikler. Gerçek boyut testte ölçülür; tahmin tutmazsa aralıklar yeniden değerlendirilir.

### 2.4 Yazıcı: kod ortak, çalışma zamanı ayrı
| | A — db'ye yerel kopya | **B — `common/` içinde ortak kod, her akış kendi havuzu (öneri)** | C — `libthecore` |
|---|---|---|---|
| Kod | sink + kurulum db'ye kopyalanır (~180 satır) | `metrics_daily_sink.h` **aynen** `common/`'a; havuz+sink+logger yaşam döngüsü `common/metrics_writer.h` (PR #12'deki desen: kuyruk 64, 1 worker, sinyal maskesi, `discard_new`, istisna sızdırmaz, kapanışta logger→havuz sırası) | ortak çekirdek |
| Çalışma zamanı | — | **Her akış ayrı nesne:** game sağlık (mevcut havuz), game SQL (yeni havuz + 1 thread), db SQL (db sürecinde havuz + 1 thread). Ayrı kuyruk, worker, dosya, `discard` ve yazma hatası sayacı → biri diğerini bekletmez, birinin kaybı ötekinin sayacına girmez | syslog ile aynı arıza alanı; **varsayılan olamaz** |
| game'in mevcut kodu | dokunulmaz | `CServerMetrics` aynı ayarlarla `metrics_writer`'ı kullanır (mekanik) | — |
| Kanıt | — | sağlık satırı biçimi bayt bayt aynı; PR #12 arıza testleri (dizin yazılamaz, kuyruk doygun, kapanış) yeniden geçer | — |
**Öneri B.** Paylaşılan sadece kod; hiçbir kuyruk/thread/sayaç paylaşılmaz. Kalan izolasyon sınırı: aynı süreçteki iki
yazıcı **aynı diske** yazar; disk takılırsa ikisinin worker'ı da takılır, ama ikisi de `discard_new` olduğundan oyun
döngüsü beklemez (bu sınır A'da da aynı). game sağlık kodunun mekanik değişikliği PR #12 testleriyle kanıtlanamazsa B-lite'a
(sadece sink ortak, db'de yerel kurulum) dönülür.

## 3. Alan sözleşmesi (`schema=1 src=sql`)
Önek: `<zaman> schema=1 src=sql kind=sum|conn host=<host|db> pid=<pid> uptime_s=<n>`.
**Değer türleri:** `sum` satırında **Δ** = son 10 sn'lik pencere (`window_ms` gerçek süre; `first=1` ilk pencere, süreç
başından beri); `conn` satırında **`*_total`** = süreç başından beri kümülatif. **anlık** = yazma anı. Süreç yeniden
başlarsa `pid` değişir, toplamlar 0'dan başlar (okuyucu `pid` değişimini sınır sayar; aynı `pid`'de toplam geri gitmez).

### `kind=conn` (bağlantı başına)
| Alan | Tür | Anlam |
|---|---|---|
| `owner`, `target`, `role` | etiket | ör. `owner=db target=player role=main`, `owner=accountdb target=common role=direct` |
| `reason` | etiket | `anomaly` ya da `periodic` |
| `q`, `cq`, `rq` | anlık | ana / kopya (takılı dahil) / sonuç kuyruğunda bekleyen (`direct`'te yok) |
| `oldest_ms` | anlık | bitmemiş en eski sorgunun kuyruğa girişinden beri (±10 ms; yoksa 0) |
| `stuck_ms` | anlık | baştaki sorgu ne zamandır başarısız olup tekrar bekliyor (yoksa 0) |
| `worker` | anlık | worker thread çalışıyor mu (ilk bağlantı başarısızsa 0, kuyruk büyür) |
| `pushed_total`, `ok_total`, `err_total`, `retry_total` | kümülatif | kuyruğa giren / **hatasız biten** / **hata koduyla biten (bu yazma uygulanmadı)** / tekrar denenecek başarısız deneme |
| `reconnect_seen_total` | kümülatif | sorgudan önce bağlantı thread id'sinin değiştiği **görüldü** (kopma anı değil; sessiz yeniden bağlanma dahil) |
| `e2006_total e2013_total e2014_total e1205_total e1213_total e_other_total` | kümülatif | başarısız **denemeler** hata koduna göre; toplam = `err_total + retry_total` |
| `exec_n_total`, `exec_us_total` | kümülatif | biten sorguların son denemesinin süresi (kilit bekleme + çalışma + ağ; ayrılamaz) |
| `exec_max_us` | önceki `conn` satırından beri | en uzun tek sorgu (`direct`'te `exec_max_ms`, ±10 ms) |
| `unexecuted_at_quit` | sayı | db kapanış satırında: kopya kuyruğunda kalıp **hiç çalıştırılmayan** sorgu; game'de `NA`. Oyuncu verisi kaybı anlamına gelmez (bkz. 1.2) |

### `kind=sum` (süreç başına, her 10 sn)
`window_ms first conns q cq rq oldest_ms_max stuck_conns workers_down` (anlık) ·
`pushed ok err retry reconnect_seen e2006 e2013 e2014 e1205 e1213 e_other exec_n exec_us exec_max_us direct_n direct_err direct_max_ms` (Δ) ·
db ek: `save_player_ok save_player_err save_item_ok save_item_err destroy_item_ok destroy_item_err save_quest_ok
save_quest_err save_safebox_ok save_safebox_err award_taken_ok award_taken_err` (Δ; **sonucu `AnalyzeQueryResult`'a
ulaşan** SAVE'ler; kapanış flush'ı dahil değil — bkz. 1.2) · kendi kaybı: `metrics_dropped metrics_write_errors`.

## 4. Sıcak yol maliyeti
| Yol | Thread | Bugün | Eklenen | Ayırma / I/O / mutex / ek sorgu |
|---|---|---|---|---|
| `AsyncQuery`/`ReturnQuery` | çağıran (game döngüsü) | 2 ayırma, string kopyası, mutex, `notify` | 1 ucuz saat (VM 63 ns) + 1 relaxed `fetch_add` + mevcut kilit altında 1 atomik yazma (kuyruk boşken) | yok |
| `DirectQuery` | çağıran (game döngüsü) | ağ gidiş-dönüşü | 2 ucuz saat + 2–3 relaxed atomik | yok |
| `PopResult` | çağıran | mutex | 1 relaxed `fetch_add` | yok |
| worker, sorgu başına | worker | sorgu + 2 `steady_clock` | 3–5 relaxed atomik; hata anında 1 ucuz saat | yok |
| db `AnalyzeQueryResult` | db ana | sonuç işleme | 1 karşılaştırma + 1 düz artırma (SAVE ise) | yok |
| pencere sonu (10 sn) | game/db ana | — | bağlantı başına ~20 atomik okuma, `snprintf`, kuyruğa ekleme | kilit yok; I/O worker'da |
Beklenti: üretici başına ≤ 0,2 µs + bir ucuz saat. **Mikro-ölçüm ne çıkarsa o kabul edilir**; daha yüksekse sebebi
araştırılır, test hedefe uydurulmaz.

## 5. Davranış değişmezliği
Dokunulmayan: kuyruk/retry/`count` döngüsü, `CopyQuery`, kapanış döngüsü, log satırları, `CountQuery`/
`CountQueryFinished`/`ResetCounter`, `CLIENT_MULTI_STATEMENTS`, `MYSQL_OPT_RECONNECT`, game kapanış sırası ve bekleme
döngüsü (`game/main.cpp:364-382`), `AnalyzeQueryResult`'ın akışı, game `metrics_*.log` biçimi.
Kanıt: 1a aracı 12 senaryo × 10, önce (bugünkü kod) ve sonra; sayaç dışı çıktılar aynı.

## 6. Arıza davranışı
- Sayaç yanlış/yarışlı → sadece telemetri yanlış; hiçbir karar sayaçları okumaz.
- Anlık alanlar ayrı ayrı okunur (grup atomik değil) → bir sorgu kadar tutarsızlık olabilir; kabul edilir.
- Yazıcı: kuyruk dolu → satır atılır (`metrics_dropped`); disk/izin hatası → atılır ve sayılır; game/db beklemez. Havuz
  kurulamazsa satırlar kapalı, süreç devam eder (tek `sys_err`). Worker'lar bütün sinyalleri maskeler.
- Hassas veri yok: SQL metni, hesap/karakter adı, IP yazılmaz; config'teki DB adı değil sadece `target` rolü yazılır.

## 7. Etki analizi
```text
Etki analizi — Risk: Yüksek (libsql = game ve db'nin bütün DB yolu; iki binary yeniden derlenir ve dağıtılır)
Etkilenen: libsql/AsyncSQL.{h,cpp} (sayaçlar, GetStats, SQLMsg.enqueue_ms, Quit sonrası sayım); common/ (sink taşınır +
  metrics_writer); game: server_metrics.cpp (writer bileşeni, mekanik), yeni sql_metrics.{h,cpp}, db.h/log.h (GetStats
  erişimi), main.cpp (çağrı noktaları); db: yeni db_metrics.{h,cpp}, ClientManager.cpp (AnalyzeQueryResult girişinde sayma,
  10 sn bloğundan çağrı), Main.cpp (kapanış satırı, METRICS_ENABLE); tools/sql-reliability (doğrulama)
Bağlı sistemler: oyuncu/item kaydı (db player/main), log yazımı, hesap/giriş sorguları, game döngüsü (DirectQuery)
Regresyon riski: davranış kodu değişmez; 1a 12 senaryo önce/sonra aynı; game sağlık satırı bayt bayt aynı
Crash riski: libsql'de ayırma/istisna eklenmez; yazıcı istisna sızdırmaz; yeni thread'ler sinyal maskeli; SQLMsg boyutu
  değişir → game, db, tools birlikte derlenir (statik kütüphane)
Yük (CPU/RAM/ağ/DB): sorgu başına birkaç relaxed atomik + 1 ucuz saat (ölçülecek); MariaDB'ye ek sorgu yok; +1 thread/süreç;
  disk ~18 MB/gün tahmini (ölçülecek)
Güvenlik/exploit: Etkilenmiyor — oyuncu girdisi yok; çıktıda SQL metni/kimlik yok
Ekonomi/dupe: Etkilenmiyor — sadece sayma; kayıt yolu ve sonuç işleme aynı
Migration: Gerekmiyor / Eski veriyle uyum: Etkilenmiyor
Rollback: METRICS_ENABLE 0 (satırlar), önceki game+db binary'leri; git revert
Test: bölüm 8
Monitoring/log: log/sql_*.log; m2metrics.py okuyucu; metrics_dropped/write_errors kendi kaybını raporlar
Panel: key=value, schema=1, owner/target/role etiketleri
Doküman: monitoring.md, bu belge, status, roadmap 1.5, worklog   Teknik borç: game kapanış kaybı ve kapanış flush SAVE sonucu
  ölçülemiyor (adım 2'de kapanış düzeltmesiyle)
Alternatif değerlendirildi mi: evet — yazıcı A/B/C; ortak havuz (metrics_dropped anlamını değiştirir, reddedildi); SAVE
  sayımı switch içinde (cache flush'ı kaçırır, reddedildi); CountQuery kilidi (atomik ile değiştirildi); parti zamanıyla
  yaş (tıkanmayı göremez, reddedildi)
```

## 8. Test planı ve kabul kriterleri
1. **Derleme:** game, db, `tools/sql-reliability` 0 yeni uyarı; db'nin spdlog başlıklarını gördüğü doğrulanır.
2. **Davranış aynı (ana kriter):** 1a 12 senaryo × 10, önce ve sonra; sayaç dışı çıktılar aynı.
3. **Sayaçlar gerçekle uyuşuyor** (1a aracı `GetStats()` + kendi `CopySize()`'ı): S1 `cq=1`, `stuck_ms>0`, `retry_total≥1`;
   S3 tekrar sayısı 1→3→6→10 ile aynı; S4 `err=1 e_other=1`; S5 `e1205=1 err=1`; S6 `e1213=1`; S7 `e2013=1`; S8 hata yok,
   `reconnect_seen=1`; S9 `unexecuted_at_quit=4`; S11 `e2014=4 err=4`; her senaryoda kovalar = `err+retry`.
4. **SAVE sayımı:** test VM'de oyuncu girişi + item taşıma + çıkış → `save_player_ok`/`save_item_ok` artar; cache flush
   kaynaklı (`dwIdent=0`) kayıtların sayıldığı log ile çapraz kontrol edilir.
5. **Maliyet:** `AsyncQuery` mikro-ölçümü önce/sonra (ns); worker ek süresi; pencere sonu süresi. Ne çıkarsa raporlanır.
6. **İzolasyon:** game SQL yazıcısının dizini yazılamaz / kuyruğu doygun → game sağlık satırları ve `metrics_dropped`
   değişmez, oyun çalışır; tersi de. db yazıcısı bozulunca db çalışır. PR #12 arıza testleri yeniden.
7. **Uçtan uca (test VM; servis yeniden başlatma ayrı onayla):** game sağlık satırı biçimi aynı; db kapanış satırı
   (`unexecuted_at_quit=0` normalde); boşta `late_pulses` önce/sonra (fark beklenmez, ölçülür).
8. **Hacim:** 1 saatlik gerçek boyut, günlük tahmin worklog'a.

## 9. Bilinen sınırlamalar
- `exec_*` kilit beklemesini çalışmadan ayıramaz. Aria'da uzun süre kilide **işaret eder, kanıtlamaz**; 1b'nin
  `table_locks_waited`'ı ile birlikte okunur.
- `oldest_ms`, `direct_max_ms` ±10 ms.
- Kapanış flush'ındaki SAVE'lerin sonucu QID bazında görülemez (sonuçları hiç işlenmiyor); game'de kapanışta çalıştırılmayan
  sorgu sayısı ölçülemez. İkisi de adım 2'deki kapanış düzeltmesine bağlı.
- Hatalı SAVE ≠ kalıcı kayıp (1.2). Kalıcı kayıp için ayrıca kanıt gerekir (ör. ticaret + trigger testi, ayrı onay).
- `reconnect_seen` yeniden bağlanmanın görüldüğü sorguyu sayar; aynı sorgunun iki kez gönderilip gönderilmediğini göremez.

## 10. Uygulama sırasında bulunanlar (tasarımdan sapmalar)
- **`uiSQLErrno` tekrar sonrası başarıda temizlenmiyor (yeni bulgu, Kanıtlı kod yolu + 1a S12).** Worker başarısız
  denemede set ediyor (`libsql/AsyncSQL.cpp`, `mysql_real_query` hata dalı), başarılı tekrarda sıfırlamıyor. Bu alanı okuyan
  tek yer `QID_LOGIN_BY_KEY` (`db/ClientManagerLogin.cpp:137`): tekrar sonrası başarılı olan sorguda `LOGIN_NOT_EXIST`
  döner. 1c davranışı değiştirmez; SAVE sayımı için `SQLMsg`'e **bilgi amaçlı** `uiFinalErrno` (mesajı bitiren denemenin
  kodu; 0 = uygulandı) eklendi. Düzeltme adım 2'nin işi.
- **Kapanış döngüsü kuyruktan doğrudan tüketiyor** (`ChildLoop` sonu): ana kuyruk uzunluğu `pushed − taken − mainDone`
  ile hesaplanır; orada çalışan/atlanan ifadeler `ok`/`err`'e girer (çalıştırıldılar), süresi ölçülmez.
- **Başlık sırası tuzağı (upstream):** `libsql/AsyncSQL.h`'deki `#define QUERY_MAX_LEN` `common/length.h`'deki aynı adlı enum
  değerini bozar; AsyncSQL `length.h`'den önce gelirse derleme kırılır. `server_metrics.h` bu yüzden raporlayıcıyı ileri
  bildirimle tutar; `sql_metrics.h` sadece `.cpp`'lerde, diğer başlıklardan sonra dahil edilir.
- **Sink temizliği akışları karıştırmaz:** `purge_` sadece kendi önekiyle ve tam uzunlukla eşleşen dosyaları siler
  (`common/metrics_daily_sink.h`); aynı `log/` dizinindeki `metrics_*` ve `sql_*` birbirine dokunmaz.
- **Kurulmamış bağlantılar:** auth çekirdeğinde `LogManager` kurulmaz (`game/config.cpp:916`); `SQLStats.configured`
  (Setup çağrıldı mı) ile satır üretmez.
- **Sıralama garantisi:** sayaç artışları `release`, okumalar `acquire`; okuyucu önce sonraki aşamayı (tamamlanma), sonra
  öncekini (kuyruğa giriş) okur → türetilen kuyruk uzunlukları negatif olmaz (x86-64'te relaxed ile aynı komut).
- **Testte deterministik olmayan alan:** ilk koşuda S9b 1/10 çıktı. Davranış satırı 10 koşuda da aynıydı
  (`markers=200`); farklı olan `stats` satırındaki `exec_n` (16 ile 200). `Quit()` geldiğinde normal döngünün kaç sorgu
  bitirdiği thread zamanlamasına bağlı, kalanı süresi ölçülmeyen kapanış döngüsü çalıştırıyor. Araçta değer yerine
  tutarlılık kontrolü (`exec_n_le_ok`) basılıyor; S9b 10/10.

## 11. Sonuçlar (2026-10-06, test VM)
| Kriter | Sonuç | Kanıt |
|---|---|---|
| Derleme | game, db, araçlar; değiştirilen dosyalarda 0 uyarı (toplam 372, baseline ile aynı) | `/root/build-1c.log` |
| **Davranış aynı (ana kriter)** | 12 eski senaryonun davranış satırları önce/sonra birebir aynı; 13 senaryo 10/10 | `tools/sql-reliability/run.sh`, `stats` satırları çıkarılıp baseline ile `diff` |
| Sayaçlar gerçekle uyuşuyor | S1/S2/S3/S9/S12 takılmada `stuck=1`; S3 tekrar 1→3→6→10; S4/S5/S6/S7 doğru kova ve `err=1`; S8 `reconnect_seen=1`; S9 `unexecuted_at_quit=4`; S11 `e2014=4 err=4`; her satırda kovalar = `err+retry`, `q`/`cq` legacy görünümle aynı | aynı koşu |
| S12 (yeni bulgu) | tekrar sonrası başarılı sorgu `uiSQLErrno=1133` taşıyor, `uiFinalErrno=0`, uygulandı | bölüm 10; adım 2'ye taşındı (`db-step1-measurement.md` §9) |
| Üretici maliyeti | `AsyncQuery` medyan 75–79 → 129–131 ns (**+50–55 ns**, iki koşu, 9×100k); `SQLStatsClock` 55–60 ns; `CollectStats` 53–63 ns/bağlantı/10 sn. Eklenen ≈ bir ucuz saat okuması; hedef (≤ 0,2 µs + bir ucuz saat) içinde. VM saati gürültülü, tek koşular değil medyan | `tools/sql-reliability/enqueue-bench.cpp` (aynı kaynak eski ve yeni libsql ile) |
| Akış izolasyonu | `sink-test` 10/10: aynı süreçte biri tıkanınca (4.932 satır düştü) diğeri 20/20 yazdı, sayaçları 0; biri yazamayınca diğeri etkilenmedi; disk dolu, gece yarısı, sinyaller, saklama | `tools/metrics/sink-test.cpp` |
| Uçtan uca (gerçek oyun) | syserr 6 süreçte deploy öncesiyle aynı; game sağlık satırı 30 alan aynı; etiketler doğru (auth'ta logmanager yok); cache flush SAVE'leri sayıldı: item 27 = MariaDB `REPLACE` 27 (dbstat), oyuncu 1 = `UPDATE` 1; db kapanışında 12 `final` satırı, `unexecuted_at_quit=0` (bu koşulda; genelleme değil, 1a S9 aksini gösteriyor); kapanış flush'ı bağlantı düzeyinde `ok=6`, `save_*`'da yok (bilinen sınır) | test VM, 13:33–13:44 |
| Bir saatlik saha kontrolü (oyuncusuz) | CPU % (bir çekirdek), eski binary 12:33–13:33 → yeni 13:46–14:46: channel1_core1 1,369 → 1,273; core2 1,348 → 1,322; core3 1,391 → 1,256; channel99 1,217 → 1,166; auth 1,220 → 1,184; db 0,597 → 0,553. Eski saatte VM'de 1a koşuları ve derleme vardı: **fark gürültü, "daha hızlı" sonucu çıkarılmaz**; ölçülebilir artış görülmedi. Maliyetin kanıtı mikro-ölçüm. Hacim: bu ölçümde 6 süreç **1,08 MB/saat, yaklaşık 24 MB/gün** (game ~3,9, db ~6,4 MB/gün; 15 dosya ≈ 370 MB); `sum` satırı ~400 B/10 sn; yük altında ölçülmedi. anomaly 0, `metrics_dropped` 0, `metrics_write_errors` 0 | dbstat `kind=proc` `cpu_us`; `log/sql_*.log` |
