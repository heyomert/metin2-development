# DB adım 1: ölçüm altyapısı ve hata enjeksiyonu — etki analizi

**Durum: TASLAK, onay bekliyor (2026-10-06).** Kod değiştirilmedi. Üst plan: `docs/engineering/db-standard.md` (bölüm 7).
Kaynak yollar `server-src/src/` altındadır. Etiketler: **Kanıtlı**, **Bilinmiyor**, **Öneri**.

**Temel şart:** gözlem sistemi gözleneni değiştirmez. Sorgu sırası, retry davranışı, kuyruk semantiği, oyun döngüsü ve
MariaDB yükü ölçümden ölçülebilir şekilde etkilenmemeli. Ölçüm hata verirse oyun/DB etkilenmez, sadece ölçüm eksik kalır.

## 1. Adımın parçaları ve sırası (revize)
| Parça | Ne | Üretim koduna dokunur mu | Sıra gerekçesi |
|---|---|---|---|
| **1a** | Hata enjeksiyonu test aracı: bugünkü davranışı deterministik üretir | Hayır (`tools/`) | Sıfır risk; 1b'nin "davranış değişmedi" kanıtı ve 2. adımın testi bu araçla |
| **1b** | MariaDB + işletim sistemi toplayıcısı (ayrı süreç) | Hayır (`deploy/`) | Oyun/DB binary'si değişmez; taban çizgi hemen başlar |
| **1c** | `AsyncSQL` ve `db` içi gözlem sayaçları | Evet (`libsql`, `game`, `db`) | Sadece gerçek kuyruk/kopya kuyruğu/yaş bilgisi süreç içinden görülebilir. 1a ile "aynı davranış" doğrulanarak |

## 2. Mevcut durum (Kanıtlı)
- **game:** sağlık kaydı var (`game/server_metrics.*`, `docs/monitoring.md`); SQL tarafı yok. `game`'de 5 `AsyncSQL` örneği:
  `DBManager::m_sql`, `m_sql_direct` (`game/db.h:103-104`), `AccountDB::m_sql`, `m_sql_direct` (`game/db.h:176-177`),
  `LogManager::m_sql` (`game/log.h:57`).
- **db:** sağlık kaydı yok. Slot başına üç örnek (`db/DBManager.h:81-83`): `m_mainSQL` (ReturnQuery), `m_asyncSQL`,
  `m_directSQL`; slotlar player/account/common/hotbackup. 10 sn'lik blok var ama içi yorumda, `g_query_count` her 10 sn
  sıfırlanıyor ve hiçbir yere yazılmıyor (`db/ClientManager.cpp:2711-2759`). Saatlik `usage.txt` var (`:2595-2620`).
- **AsyncSQL'de dışarıdan görülebilen:** `CountQuery()` (sadece `m_queue_query`), `CountResult()`, `CountQueryFinished()`,
  `GetCopiedQueryCount()` (`libsql/AsyncSQL.cpp:299-324`). **Görülemeyen:** kopya kuyruğunda bekleyen (`m_queue_query_copy`,
  sadece worker thread'i dokunuyor, kilitsiz → dışarıdan okumak veri yarışı), en eski bekleyenin yaşı, hata/retry/atılan
  sayıları, hata kodu dağılımı, tıkanma.
- **MariaDB (Kanıtlı, 11.8.9):** `Innodb_deadlocks`, `Innodb_row_lock_waits/time/time_max/current_waits`,
  `Table_locks_waited/immediate`, `Slow_queries`, `Threads_running/connected`, `Questions`, `Com_*`, `Innodb_data_fsyncs`,
  `Innodb_os_log_written`, buffer pool sayaçları mevcut. Şu an: `Table_locks_waited=5`, `Innodb_deadlocks=0`,
  `Threads_connected=37`. `performance_schema=OFF` (açmak yeniden başlatma + bellek; tasarım ona dayanmıyor).
  `slow_query_log=0`, `long_query_time=10`.
- **Saat maliyeti (VM, ölçüldü):** `CLOCK_MONOTONIC` 12.221 ns/okuma; `CLOCK_MONOTONIC_FAST` 63 ns/okuma, adım ~3,6–10 ms.

## 3. Neyi ölçeceğiz
### 1b — MariaDB/OS toplayıcısı (10 sn, ayrı süreç)
`SHOW GLOBAL STATUS`'tan seçili sayaçların **pencere farkları** (ör. `row_lock_waits`, `row_lock_time_ms`, `deadlocks`,
`table_locks_waited`, `slow_queries`, `questions`, `com_insert/replace/update/delete/select`, `data_fsyncs`,
`log_written_bytes`, `bp_reads`), anlık `threads_running/connected`, `row_lock_current_waits`, `row_lock_time_max_ms`;
`procstat` ile game/db/mariadbd CPU (user+sys) ve RSS; disk I/O (`iostat`). Satır: `metrics_YYYY-MM-DD.log` biçiminde
`schema=1 host=mariadb ...` → `m2metrics.py` aynı şekilde okur.

### 1c — Süreç içi SQL sayaçları (her `AsyncSQL` örneği için)
| Alan | Anlam | Nasıl (maliyet) |
|---|---|---|
| `q` | ana kuyrukta bekleyen | mevcut `CountQuery()` (emit anında 1 kilit) |
| `cq` | **kopya kuyruğunda bekleyen** (takılı olan dahil) | worker'ın güncellediği atomik |
| `rq` | sonuç kuyruğunda | mevcut `CountResult()` |
| `oldest_ms` | en eski bekleyenin yaşı (üst sınır) | sorgu kuyruğa girerken `CLOCK_MONOTONIC_FAST` damgası (63 ns); kopya kuyruğunun başındaki damga worker'ca atomiğe yazılır |
| `stuck_ms` | baştaki sorgu ne zamandır başarısız | ilk başarısızlıkta damga; başarı/atılmada sıfır |
| `ok` `err` `retry` `drop` | pencere sayaçları | atomik (relaxed) |
| `e2006 e2013 e1205 e1213 e1062 e1064 e1138 e_other` | hata kodu kovaları | atomik; sabit dizi |
| `sel ins rep upd del oth` | ifade sınıfı (ilk kelime) | ilk birkaç karakter karşılaştırması; **SQL metni telemetriye gitmez** |
| `exec_n exec_sum_us exec_max_us slow` | çalışma süresi | worker'ın **mevcut** profiler okumaları (`AsyncSQL.cpp:401, 444`), ek saat okuması yok |

`DirectQuery` (kuyruksuz, çağıranın thread'i): `ok/err` + hata kovaları + süre (mevcut `DBManager::DirectQuery`
ölçümü `game/db.cpp:65-72`, `db/DBManager.cpp:119-126` zaten saat okuyor).

**db'ye özel (Öneri):** `*_SAVE` sonuçlarında `uiSQLErrno != 0` sayısı, QID başına (`db/ClientManager.cpp:2557-2562`,
davranış değişmeden sadece sayma); önbellek boyutları (oyuncu/item) ve pencere içindeki flush sayısı.

**Hacim:** her pencerede süreç başına bir **özet** (toplam `q`, `cq`, en büyük `oldest_ms`, `err`, `retry`, `drop`,
`stuck`), ana sağlık satırına eklenir. Örnek başına **ayrıntı satırı sadece** o pencerede hata/retry/atılma/tıkanma ya da
`oldest_ms` eşiği aşımı varsa yazılır. Normal günde hacim neredeyse değişmez. Tahmin değil: 1c testinde ölçülecek.

**Panel/monitoring uyumu:** hepsi `anahtar=değer`, `schema` alanlı, adıyla okunur (`docs/monitoring.md` sözleşmesi);
Prometheus/Grafana/Admin Panel aynı dosyaları okuyabilir. game/db hiçbir dış servise bağlanmaz, panele bağımlı olmaz.

## 4. Dokunulacak dosyalar
- **1a:** `tools/sql-reliability/` (C++ test programı + çalıştırma script'i; `libsql`/`libthecore`'a bağlanır, geçici
  MariaDB'ye `127.0.0.1:<ayrı port>` ile bağlanır). Üretim koduna dokunmaz.
- **1b:** `deploy/freebsd/metrics/m2dev-dbstat.sh` (+ rc servis dosyası ya da `daemon(8)`), `tools/metrics/m2metrics.py`
  (yeni kaynak), `docs/monitoring.md`.
- **1c:** `libsql/AsyncSQL.h/.cpp` (sayaçlar + `GetStats()` anlık görüntüsü; kuyruk mantığı **aynı**), `game/server_metrics.cpp`
  (özet + ayrıntı satırı), `game/metrics_daily_sink.h` → `common/`'a taşınır (db de kullanır), yeni `db/db_metrics.{h,cpp}` +
  `db/ClientManager.cpp` 10 sn bloğuna çağrı, `db/ClientManager.cpp` save sonuçlarını sayma, `docs/monitoring.md`.
- **1c'de bilerek dokunulmayan:** retry listesi, `count` döngüsü, kapanış, `CLIENT_MULTI_STATEMENTS` (2. adım ve ayrı
  değişiklik).

## 5. Performans ve etki alanı riski
| Parça | Etki alanı | Risk | Önlem / kanıt |
|---|---|---|---|
| 1a | yok (test) | geçici MariaDB aynı VM'de CPU/disk | oyuncusuzken; geçici örnek her testten sonra silinir |
| 1b | MariaDB'ye +1 bağlantı, 10 sn'de 1 `SHOW GLOBAL STATUS` | çok düşük; tablo kilidi almaz | süresi ölçülecek; MariaDB kapalıysa satır yazmadan bekler |
| 1c | **game + db, her DB sorgusu** | (a) maliyet: sorgu başına 1 ucuz saat + birkaç atomik; (b) yanlışlıkla davranış değişikliği | (a) mikro-ölçüm + A/B; (b) 1a testinin sonuçları değişikliğin önce/sonrasında **birebir aynı** olmalı |

1c bir `libsql` değişikliği: `game` ve `db` birlikte yeniden derlenip dağıtılır. Geri dönüş: önceki binary'ler.

## 6. Arıza durumları
- Sayaç yarışı / yanlış değer → sadece telemetri yanlış; kuyruğa dokunulmadığı için davranış etkilenmez.
- Kopya kuyruğu boyutu dışarıdan okunursa veri yarışı → okunmaz, worker'ın yazdığı atomik okunur.
- Emit sırasında `m_mtxQuery` kilidi → producer ile kısa çakışma (10 sn'de 1; mevcut `CountQuery` ile aynı).
- `CLOCK_MONOTONIC_FAST` FreeBSD'ye özgü → Linux'ta `CLOCK_MONOTONIC_COARSE`, diğerlerinde `steady_clock` (derleme
  zamanı seçimi). Kaba çözünürlük: `oldest_ms` ±10 ms; kabul edilebilir.
- db yazıcısı game'deki izole sink'i kullanır (kendi havuzu, `discard_new`, istisna sızdırmaz) → disk dolu/izin hatası db'yi
  bekletmez (`docs/monitoring.md` → Arıza davranışı).
- Toplayıcı (1b) MariaDB'ye bağlanamazsa hata satırı yazar ve bekler; MariaDB'yi zorlamaz (sabit aralık, yeniden deneme
  fırtınası yok).
- Hassas veri: SQL metni, hesap/karakter adı, IP yazılmaz.

## 7. Test / hata enjeksiyonu planı (1a)
Ortam: geçici MariaDB (ayrı datadir, `127.0.0.1:<port>`, canlıya dokunmaz). Program `CAsyncSQL`'i doğrudan kullanır.
Kanıt kaynağı: veritabanındaki işaretçi satırlar (hangi sorgu gerçekten uygulandı), `CountQuery/CountQueryFinished`, syserr.

| # | Senaryo | Nasıl üretilir (deterministik) | Bugünkü kodda beklenen (koddan) |
|---|---|---|---|
| S1 | Tek sorguluk partide geçici hata | bağlantıyı sunucudan `KILL` → sonraki sorgu 2006 | hiç tekrar denenmez; kopya kuyruğunda kalır; worker uyur |
| S2 | S1 + yeni sorgu gelir | S1 sonrası işaretçi `INSERT` | takılı sorgu `count` kez denenir; yeni sorgu arkasında bekler / ilerler |
| S3 | Çok sorguluk partide geçici hata | 5 sorgu, ilki 2006 | ilki en fazla 5 kez denenir; arkadakiler bekler |
| S4 | Retry listesindeki **kalıcı** hata | NULL içeren sütuna `ALTER ... NOT NULL` → 1138 | kuyruk süresiz tıkanır; her yeni partide aynı sorgu `count`×100 ms |
| S5 | Listede olmayan kalıcı hata | sözdizimi hatası (1064) | log + atılır; arkadakiler çalışır |
| S6 | 1205 | ikinci oturum satırı `FOR UPDATE` ile tutar; test oturumu `innodb_lock_wait_timeout=1` | log + atılır (retry yok) |
| S7 | 1213 | iki oturum satırları ters sırayla kilitler (`SLEEP` ile sıralı) | log + atılır |
| S8 | 2013 | uzun sorgu sırasında `KILL CONNECTION` | log + atılır; uygulanıp uygulanmadığı işaretçiyle kontrol |
| S9 | Kapanış, kopya kuyruğunda takılı varken | S4 + arkasına işaretçiler + `Quit()` | takılı ve arkadakiler **yazılmaz**; `CountQuery()` 0 gösterir |
| S10 | 1205 vs 1213 farkı (çok ifadeli transaction ile) | `BEGIN; UPDATE a; UPDATE b(1205/1213); COMMIT` | 1205: sadece ifade geri alınır, transaction açık kalır; 1213: bütün transaction geri alınır (MariaDB davranışının kendisi) |

Her senaryo 10 kez; sonuçlar her seferinde aynı olmalı (deterministik). Bugünkü kodda "beklenen" sütunu **gözlenen**le
karşılaştırılır; bunlar 2. adımın düzeltmesinde tersine dönmesi gereken testlerdir.

**Ticaret "dupe" iddiası (ayrı test, test VM, ayrı onay — DB'ye geçici trigger):** A, X'i B'ye verir; `item` tablosuna
o item için yazmayı başarısız kılan geçici bir trigger (`SIGNAL`, errno 1644, retry listesinde değil); çıkış + yeniden
başlatma; DB ve oyun içi durum. Kod okumama göre beklenen: X tekrar A'da, B'de yok; item **çoğalmaz** (önbellek item
kimliğiyle tek kayıt, `db/ClientManager.cpp:1360-1420`) → sonuç büyük olasılıkla "eski duruma dönme/değer kaybı", item
dupe'u değil. Önceki "dupe" ifadem **kanıtlanmadı**; bu test doğrular ya da yanlışlar.

## 8. Kabul kriterleri
- **1a:** S1–S10 her biri 10/10 aynı sonucu verir; gözlenen bugünkü davranış belgelenir (worklog).
- **1b:** 10 sn'de bir satır; `SHOW GLOBAL STATUS` süresi ölçülür ve raporlanır; MariaDB kapalıyken toplayıcı çökmez,
  game/db etkilenmez; `m2metrics.py` okur.
- **1c:** (i) 1a sonuçları instrumentation öncesi/sonrası **birebir aynı**; (ii) sayaçlar 1a'daki gerçekle uyuşur (S1/S4'te
  `cq>0`, `stuck_ms` ve `oldest_ms` artar; S5–S8'de doğru hata kovası); (iii) mikro-ölçümde sorgu başına ek maliyet
  ölçülür ve raporlanır (hedef: ≤ 0,2 µs + bir ucuz saat); (iv) A/B: boşta ve sentetik DB yükünde süreç CPU'su ve
  `late_pulses` gürültü içinde; (v) SQL metni çıktıda yok; (vi) normal günde dosya boyutu artışı ölçülür.
- Hepsi: `docs/monitoring.md` alanları, worklog, ilgili testlerin komutları.

## 1a sonuçları (2026-10-06, test VM, geçici MariaDB)
Araç: `tools/sql-reliability/` (`sqlrt.cpp` + `run.sh`). **MariaDB Server 11.8.9, Connector/C 3.4.5** (repodaki gömülü
`libmariadbclient.a`; `game` ile aynı `libsql`/`libthecore`). Her senaryo ayrı süreç, ayrı klasör, sıfırdan şema; 10 tekrar.
**12/12 senaryo 10/10 aynı.** Gözlenen davranış otoritedir; plandaki varsayımlardan sapmalar aşağıda.

**Koruma (fail-closed):** araç bağlantı keser, kilit/kilitlenme ve hata üretir, veritabanı siler. Hiçbir yazmadan önce:
yönetici bağlantısının datadir'i `/var/tmp/m2sqlrt/`; `run.sh`'ın her çalıştırmada yazdığı rastgele token o sunucuda; TCP
üzerinden bağlanan AsyncSQL aynı sunucuda (yönetici bağlantısının işlem listesinde görünür). `run.sh` port kullanımdaysa
başlamaz ve her çalıştırmada korumayı öz-test eder (token yok / yanlış token / canlı sunucunun soketi → reddedilmeli).
Test edildi: üçü de reddedildi, `RT_PORT=3306` ile başlamadı, canlı sunucuda önce/sonra değişiklik yok.

Enjeksiyon yöntemleri ölçülerek seçildi: retry listesindeki hata için `SET PASSWORD FOR ghost@localhost` → **1133**
(kullanıcı oluşturulunca aynı ifade başarılı; uygulanma kanıtı `mysql.global_priv`). Reddedilenler: `ALTER ... NOT NULL`
(katı olmayan modda sadece uyarı + NULL→0; katı modda 1265, listede yok), `ADD PRIMARY KEY` (NULL→0, hata yok),
MyISAM dosya izni (1036, listede yok), `KILL` ile 2006 (oluşmadı, S8).

| # | Senaryo | Gözlenen (10/10) | DB kanıtı | Not |
|---|---|---|---|---|
| S1 | tek sorguluk parti, listedeki hata (1133), sonra hata kalkar | 1 deneme; takıldı (`q=0 cq=1`); hata kalktıktan 3 sn sonra hâlâ denenmedi; yeni sorgu gelince uygulandı | `applied` 0 → 0 → 1 | `retrying=1` log'u yazıldı ama tekrar olmadı (yanıltıcı log) |
| S2 | 5 sorguluk parti, aynı hata | 5 deneme; 5'i de takıldı (`cq=5`); hata kalkınca yeni sorgu gelene kadar bekledi; sonra hepsi uygulandı | `applied` 0 → 0 → 1, işaretçi 0 → 0 → 5 | |
| S3 | listedeki hata hiç kalkmaz, yeni sorgular gelir | deneme toplamı 1 → 3 → 6 → 10; `cq` 1 → 4 | işaretçi hep 0 | kuyruk kalıcı tıkalı |
| S4 | listede olmayan hata (1064) | atıldı, arkadaki çalıştı | işaretçi 1 | |
| S5 | 1205 | atıldı; kilit kalkınca tekrar denenmedi | `v1` 0 → 0 (yazma kayboldu) | |
| S6 | 1213 (autocommit `AsyncQuery` kurban) | `Innodb_deadlocks=1`; atıldı; tekrar yok | `v1=v2=0` (kayboldu); arkadaki işaretçi 1 | |
| S7 | sorgu sırasında `KILL CONNECTION` | **2013**; atıldı; sonraki sorgu yeniden bağlanıp çalıştı | yavaş `INSERT` uygulanmadı; işaretçi 1 | |
| S8 | boştayken `KILL CONNECTION` | **2006 oluşmadı**: Connector/C sessizce yeniden bağlanıp sorguyu gönderdi; "was reconnected" bir sonraki sorguda yazıldı | işaretçiler 1, 1 | `MYSQL_OPT_RECONNECT` etkisi |
| S9 | kopya kuyruğunda takılı + 3 iş varken `Quit()` | `q=0` gösterirken `cq=4`; kapanışta hepsi atıldı | işaretçi 0 | kapanışta kayıp |
| S9b | normal 200'lük birikimle `Quit()` | hepsi yazıldı | 200/200 | |
| S10 | MariaDB: açık transaction içinde 1205 / 1213 | 1205: sadece ifade geri alındı, transaction açık, ilk ifade korundu. 1213: bütün transaction geri alındı | ilk ifade 1 / 0 | |
| S11 | `AsyncQuery` ile sonuç döndüren ifade (`SELECT 1`) | sonraki **bütün** yazmalar 2014 (out of sync) alıp atıldı; kendiliğinden düzelmedi | işaretçi 0 (4 yazma) | **yeni bulgu**; bugün üretim kodunda böyle çağrı yok (arama) |

**Plandan sapmalar / yeni bulgular:**
1. **S11:** `AsyncQuery` yolunda `Store()` çağrılmadığı için sonuç döndüren tek bir ifade bağlantıyı zehirliyor; o bağlantının
   sonraki bütün yazmaları sessizce kayboluyor. Bugün `game`/`db` async yollarında `SELECT/SHOW/CALL` yok (gizli tuzak).
2. **S8:** bağlantı kopması çoğu durumda 2006 olarak hiç görünmüyor; Connector/C yeniden bağlanıp gönderiyor. Kod içindeki
   2006 retry dalı pratikte nadiren çalışır; sorgu sırasında kopmada **2013** geliyor ve **atılıyor**.
3. `"AsyncSQL: retrying"` log'u gerçekte tekrar olmasa da yazılıyor (S1, S2). Log'a dayanarak "tekrar edildi" denemez.
4. `CountQueryFinished()` atılan (başarısız) sorguları da sayıyor (S7: `finished=1`, sorgu uygulanmadı); `CountQuery()`
   kopya kuyruğunu görmüyor (S9: `q=0`, `cq=4`). Mevcut sayaçlarla "bitti/bekleyen" doğru okunamaz → 1c'deki ayrı
   `ok/drop/cq` sayaçları gerekli.

## 9. 2. adım için not (bu adımın kapsamı değil)
Revize (1a sonuçlarına göre):
- Retry, parti sayacına (`count`) bağlı olmamalı; takılı sorgu yeni sorgu beklemeden zamanlı tekrar edilmeli (S1–S3);
  kalıcı hatalar kuyruğu kilitlememeli; kapanışta kopya kuyruğu da işlenmeli ya da kalanlar sayılıp raporlanmalı (S9).
- 1205/1213 autocommit tek ifadede güvenle tekrar edilebilir (S10: ifade/transaction geri alındı, kalıntı yok); 2013'te
  ifadenin uygulanıp uygulanmadığı belirsiz (S7'de uygulanmadı ama genel kural değil) → sadece idempotent sınıf.
- Async yol, sonuç döndüren ifadeleri ya boşaltmalı ya da reddetmeli (S11).
- Log ve sayaç anlamları düzeltilmeli (bulgu 3–4).
- 1205 (ifade geri alınır, transaction açık kalır) ile 1213 (transaction geri alınır) aynı muamele görmemeli. Autocommit
  tek ifadede ikisi de "uygulanmadı" demektir; çok ifadeli transaction'da 1213 bütün transaction'ın baştan tekrarını,
  1205 ise transaction sahibinin kararını gerektirir. Retry politikası: hata kodu + **işlemin idempotent olup olmadığı**
  + **transaction sınırı**.
- `CLIENT_MULTI_STATEMENTS` kapatma ayrı ve küçük bir değişiklik olarak (kullanılmadığı kanıtlı: bölüm 2,
  `db-standard.md`).
