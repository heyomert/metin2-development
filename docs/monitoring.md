# Sunucu sağlık kaydı (server metrics)

Her `game` süreci (auth dahil; db hariç) 10 saniyede bir tek satırlık sağlık kaydı yazar. Amaç: production'a geçmeden
önce sunucuyu gözlemlenebilir yapmak, lag/stabilite olaylarını sonradan kanıtla teşhis edebilmek ve yük testini
okuyabilmek. Kaynak: `server-src/src/game/server_metrics.{h,cpp}`; yazıcı `server-src/src/common/metrics_writer.h` +
`metrics_daily_sink.h` (game ve db'deki bütün telemetri akışları aynı kodu, **ayrı** kuyruk/worker ile kullanır);
karar ve testler: `docs/worklog/2026-10-05-server-metrics.md`.

**Agent'lar için:** "lag var mıydı / sunucu ne durumda" sorusuna önce buradan bak; serbest metin log'u (`syslog.log`)
bu soruyu cevaplamaz. Özet: `python3 tools/metrics/m2metrics.py --hours 1` (VM'de, salt okunur).

## Dosyalar
- Yol: her sürecin klasöründe `log/metrics_YYYY-MM-DD.log` (ör. `server/channels/channel1/core1/log/`).
  Aktif dosya yok, gün değişince yeni dosyaya geçilir (sürecin yerel saatiyle).
- Saklama: bugün + önceki 14 gün; daha eskisi her günün ilk satırında silinir. İlgisiz dosyalara dokunulmaz.
- Boyut (test VM'de ölçüldü, ~1.000 satır): satır 401–417 bayt, günde 8.640 satır → süreç başına ~3,4 MB/gün;
  5 süreç × 15 dosya ≈ 260 MB.
- Kişisel veri yok (IP, hesap, karakter adı yazılmaz).

## Açma / kapama
`conf/game.txt`: `METRICS_ENABLE: 0` → metrikler kapalı, ölçüm kodu tamamen atlanır (yeniden başlatma gerekir).
Satır yoksa varsayılan `1`. Açılışta `syslog.log`'a `METRICS: enabled ...` ya da `METRICS: disabled ...` yazılır.

## Satır biçimi
```
2026-10-05T19:30:14+0300 schema=1 host=channel1_1 ch=1 port=11011 pid=4242 uptime_s=600 window_ms=10003 users_local=1 ...
```
İlk alan yerel zaman (`%Y-%m-%dT%H:%M:%S%z`), gerisi `anahtar=değer`. **Alanları adıyla oku, sırayla değil**; yeni alan
eklenebilir, biçim değişirse `schema` artar.

## Alanlar
Pencere alanları son `window_ms` içindeki toplamdır; anlık alanlar satırın yazıldığı andaki değerdir. Pencere, 10 sn
dolduktan sonraki ilk tur başında (`heart_idle` döndükten hemen sonra) kapanır: o an dönen pulse'lar önceki turun
süresini kapsadığı için kapanan pencereye eklenir, böylece uzun bir tur ve yol açtığı gecikme aynı satırda görünür.

| Alan | Birim | Anlamı | Kaynak |
|---|---|---|---|
| `schema` | — | Biçim sürümü (şu an 1) | `server_metrics.cpp` `Emit` |
| `host`, `ch`, `port` | — | `HOSTNAME`, `CHANNEL`, `PORT` (sürecin `CONFIG`'i); boşluk/`=` → `_` | `g_stHostname`, `g_bChannel`, `mother_port` (`game/config.cpp`) |
| `pid`, `uptime_s` | —, sn | Süreç kimliği ve metrik başlangıcından beri süre; `pid` değişmesi = yeniden başlatma | — |
| `window_ms` | ms | Pencerenin **gerçek** süresi (hedef 10.000; duraklama/yük varsa uzar) | monoton saat |
| `users_local` | adet | Bu süreçte karakteri olan bağlantı. ~5 sn'de bir güncellenir (`main.cpp` heartbeat, `UpdateLocalUserCount`) | `desc_manager.cpp` `FuncWho` |
| `descs_total` | adet | Bütün bağlantılar: oyuncu + çekirdekler arası P2P + db/connector | `desc_manager.cpp` `AcceptDesc`/`AcceptP2PDesc`/`CreateConnectionDesc` |
| `chars_total` | adet | Bütün karakterler (oyuncu, mob, NPC…) | `CHARACTER_MANAGER::m_map_pkChrByVID` |
| `pcs` | adet | Oyuncu karakterleri | `m_map_pkPCChr` |
| `fsm_chars` | adet | Durum makinesi çalışan karakterler (oyuncu **ve** mob/NPC; "aktif mob" değildir) | `m_set_pkChrState`, `char.cpp` `UpdateStateMachine` |
| `iters` | adet | Ana döngü turu | `main.cpp` `idle` |
| `pulses` | adet | İşlenen pulse; ≈ `PASSES_PER_SEC` × süre. Gecikme olan pencerede bundan **fazla** olabilir (aşağıda "`heart_idle` fazla sayar") | `PASSES_PER_SEC: 60` (`server/share/conf/game.txt`) |
| `late_pulses` | adet | Gecikme yüzünden telafi edilen pulse toplamı (Σ geçen−1). Oyunun yaşadığı lag | `libthecore/heart.cpp` `heart_idle` |
| `late_iters` | adet | Gecikme yaşanan tur sayısı | — |
| `max_late_pulses` | adet | Tek seferdeki en büyük gecikme; gecikme ≥ değer × 16,7 ms (`PASSES_PER_SEC: 60`). 30 sn üstü `heart_idle`'da 1800'e kırpılır (`syserr`: `losing N seconds`) | `heart.cpp:78-82` |
| `work_us` | µs | Turların iş süresi toplamı (uyku hariç) | `thecore_idle` dönüşü → `io_loop` sonu |
| `work_max_us` | µs | En uzun tur. Tur bütçesi 1/60 sn ≈ 16.667 µs; aşarsa sonraki turda `late_pulses` görünür | — |
| `iter_gap_max_us` | µs | İki tur başlangıcı arasındaki en uzun **gerçek** süre (iş + uyku + süreç çalışmadığı süre). Normal 17–25 ms (test VM). **Gerçek duraklama göstergesi** | monoton saat, ek okuma yok |
| `busy_pct` | % | `work_us` / gerçek pencere süresi | — |
| `event_us` | µs | Zamanlanmış olaylar (`event_process`) | `main.cpp` `heartbeat` |
| `hb_us` | µs | Heartbeat'in geri kalanı: oyuncu sayısı/kayıt, item güncelleme, `DBManager`/`AccountDB`/PvP işleme | `main.cpp` `heartbeat` |
| `chr_us` | µs | `CHARACTER_MANAGER::Update`: oyuncu ve durum makinesi güncellemesi | `main.cpp` `idle` |
| `io_us` | µs | `db_clientdesc->Update` (zaman kontrolü, 5 dk'da bir kanal durumu) + `io_loop` (soketler, paket işleme) | `main.cpp` `idle` |
| `other_us` | µs | `work_us` − bölümler toplamı: `thecore_tick`, `get_dword_time`, sayaç sıfırlama, metrik satırının kendisi vb. Bölümler işin %100'ü değildir | — |
| `events` | adet | İşlenen olay | `event_process` dönüşü |
| `sent_bytes` | bayt | Oyunculara/peer'lara yazılan bayt | `desc.cpp` `current_bytes_written` (delta ile, kayma yok) |
| `metrics_dropped` | adet | Kuyruk dolu olduğu için atılan satır (**süreç başından beri**) | spdlog `discard_counter` |
| `metrics_write_errors` | adet | Dosyaya yazılamayan satır (**süreç başından beri**) | `metrics_daily_sink.h` |
| `build`, `build_dirty`, `build_src` | metin | Bu satırı yazan binary'nin gömülü derleme kimliği: tam commit (40 hex ya da `unknown`), `0`/`1`/`unknown`, `git`/`archive`/`injected`/`none`. Sürecin ömrü boyunca sabit; derleme değişince pencere hangi binary'ye ait doğrudan görülür. Kimliksiz eski binary'lerin satırlarında yok | `common/build_identity_impl.h` (`M2BuildFields`), `docs/build-and-run.md` → "Derleme kimliği" |

## Normal aralıklar
**BASELINE_PENDING.** Production ve yük testi ölçümü yok; aralıklar acceptance/yük/soak ölçümlerinden sonra buraya
yazılacak. O zamana kadar karşılaştırma aynı sunucunun kendi geçmişiyle yapılır.

Kesin olanlar: `late_pulses > 0` = döngü en az bir kez tur bütçesini aştı; `work_max_us > 16667` = en az bir tur
1/60 sn'den uzun sürdü (`PASSES_PER_SEC: 60` için).

Test VM'de boşta görülenler (2026-10-05, 0 oyuncu, kanıt worklog'da): `busy_pct` 1–2, `work_max_us` 1–7 ms,
`iter_gap_max_us` 17–25 ms. Açılıştan sonraki ilk 1–2 dakikada `late_pulses` 1–2 ve tek tük 20–40 ms'lik turlar normal
(ısınma). Ara sıra **bütün süreçlerde aynı anda** `late_pulses=1` görülüyor: VM genelinde kısa duraklama.

## Gecikmeyi okuma
1. **Duraklama ne kadardı?** `iter_gap_max_us`. Yaşandığı pencerede görünür.
2. **Kimin suçu?** Aynı satırda `work_max_us` ≈ `iter_gap_max_us` → döngünün kendi işi; bölüm alanlarına
   (`event_us`, `chr_us`, `io_us`…) bak. `work_max_us` çok küçük → süreç **çalışmadı** (VM/host duraklaması, CPU
   yetmedi, `kill -STOP`): aynı saatte diğer süreçlere bak; hepsinde varsa sunucu/host geneli.
3. **Oyun bunu nasıl yaşadı?** `late_pulses`/`max_late_pulses`. Süreç uyku sırasında durduysa `heart_idle` gecikmeyi
   uykudan **önce** hesapladığı için (`heart.cpp:36-67`) gecikme bir sonraki turda sayılır; araya pencere sınırı
   girerse `late_pulses` bir sonraki satırda çıkar, `iter_gap_max_us` ise duraklamanın satırında kalır.
   Örnek (`kill -STOP` 2 sn): `iter_gap_max_us=2018538 work_max_us=4811 late_pulses=120`.
4. **`heart_idle` fazla sayar (upstream davranışı):** gecikmede geçen pulse'ın üstüne +1 döner ve zaman tabanını
   "şimdi"ye kaydırır (`heart.cpp:49-55, 70`); her gecikmede oyun saati ~1 pulse öne geçer. Sık gecikmeli pencerede
   `pulses` > 60 × saniye olur (ölçülen: `late_pulses=79` → 10 sn'de 653 pulse). Metrik bunu olduğu gibi raporlar;
   düzeltilmedi (`libthecore`'u db de kullanıyor, ayrı iş).

## Ölçüm maliyeti
Tur başına 4 + 2×(işlenen pulse) `steady_clock::now()` okuması (bölümler zaman damgasını paylaşır) + pencere başına 1.
Döngü saniyede en fazla 60 tur / 60 pulse çalıştığı için bu **sabit** bir maliyettir: oyuncu ya da mob sayısıyla
artmaz (~360 okuma/sn/süreç).

Test VM'de (VirtualBox, `kern.timecounter.hardware: ACPI-fast`; `TSC-low` kalitesi 0, "TSC calibration failed") bir
okuma **~11,6 µs** (`tools/metrics/clock-cost.cpp`): her okuma bir sistem çağrısı + emüle edilen I/O portu.
**A/B ölçümü** (2026-10-05, boşta, 5 game süreci, 300 sn, `procstat -r` user+sys):

| | süreç başına CPU (bir çekirdeğin %'si) | 5 süreç toplam |
|---|---|---|
| `METRICS_ENABLE: 1` | 1,40–1,64 (ort. 1,56) | 23,4 sn / 300 sn |
| `METRICS_ENABLE: 0` | 0,77–1,06 (ort. 0,94) | 14,2 sn / 300 sn |

Fark süreç başına **~0,6 puan** (bir çekirdeğin %0,6'sı), 5 süreçte 4 vCPU'luk VM'in ~%0,8'i. Tahmin (%0,4) bundan
düşüktü. Oyunun kendi boştaki CPU'sunun da çoğu sistem zamanı (ör. core1: user 1,5 sn / sys 9,5 sn); oyun da her turda
saati birkaç kez okuyor (`heart_idle`, `get_dword_time`), aynı sebep. TSC zaman kaynağı olan fiziksel sunucuda okuma
sistem çağrısı gerektirmez, maliyetin yüzlerce kat düşük olması beklenir. **Production donanımında ölçülmedi:**
`docs/production-checklist.md` → metrik maliyeti.

## Arıza davranışı (gözlem sistemi gözleneni bozmaz)
- Yazma ayrı bir spdlog havuzunda (kuyruk 64, 1 thread, `discard_new`): kuyruk doluysa satır atılır, oyun thread'i
  beklemez. syslog/syserr'in global havuzu paylaşılmaz.
- Dizin oluşturulamazsa / dosya açılamazsa / yazma ya da `fflush` başarısız olursa (disk dolu dahil) satır atılır,
  `metrics_write_errors` artar, sonraki satırda yeniden denenir. Her satır yazılınca `fflush` edilir: satır stdio
  tamponuna sığdığı için disk dolu hatası ancak orada görünür. Sink hiçbir istisnayı dışarı sızdırmaz.
- Süreç `kill -9` ile ölürse yazılmamış son pencere (en fazla 10 sn) kaybolur; dosyada yarım satır kalmaz, yeniden
  başlatınca yeni `pid` ile aynı günün dosyasına devam edilir.
- Başlatma başarısız olursa metrikler kapalı kalır (`syserr`: `METRICS: initialization failed`), oyun devam eder.
- Metrik thread'i bütün sinyalleri engeller: `SIGVTALRM` watchdog'u ve diğer işleyiciler eskisi gibi çalışır.
- Kapanışta son kısmi pencere yazılır, kuyruk boşaltılır (`destroy()` → `thecore_destroy` öncesi).
- Test: `tools/metrics/sink-test.cpp` (`metrics_writer`, oyun gerekmez; 10 senaryo: normal yazma, kuyruk doygunluğu,
  dizin/dosya yolu hatası, saklama, gece yarısı, sinyaller, aynı süreçte iki akışın birbirinden bağımsızlığı (tıkanma
  ve yazamama), disk dolması).

# SQL (sql_*.log)

game çekirdekleri ve db, SQL bağlantılarının kuyruk, bekleme ve hata durumunu ayrı bir dosyaya yazar. Amaç: AsyncSQL'in
bugünkü davranışını (takılan kuyruk, tekrar denenmeyen hatalar; `docs/engineering/db-step1-measurement.md` → 1a
sonuçları) canlıda görünür yapmak ve düzeltme/InnoDB dönüşümünü önce/sonra karşılaştırmak. **Davranışı değiştirmez**:
sayaçlar `libsql/AsyncSQL` içinde, sorgu işleme onları okumaz. Tasarım ve kanıtlar:
`docs/engineering/db-step1c-sql-counters.md`. Kaynak: `libsql/AsyncSQL.{h,cpp}` (`CollectStats`),
`common/sql_metrics.h` (satırlar), `game/server_metrics.cpp`, `db/DBMetrics.{h,cpp}`.

- Dosya: her sürecin klasöründe `log/sql_YYYY-MM-DD.log` (game çekirdekleri ve `channels/db/log/`), 14 gün. Zaman
  damgası game sağlık satırıyla aynı yazıcıdan: UTC farkı `+03:00` biçiminde (dbstat `+0300` yazar; ikisi aynı an).
- Boyut: **bu saha ölçümünde** (test VM, oyuncusuz, 1 saat) game süreci ~3,9 MB/gün, db ~6,4 MB/gün; 6 süreç yaklaşık
  24 MB/gün, 15 dosya ≈ 370 MB. `sum` satırı her 10 sn yazılır ve alanları sabit; `anomaly` satırları olaylarla artar.
  Yük altında ölçülmedi (roadmap 2.2).
  game'in `metrics_*.log`'u değişmez. Okuma: `python3 tools/metrics/m2metrics.py --sql --hours 24`.
- Açma/kapama: game `METRICS_ENABLE` (`conf/game.txt`, sağlık satırıyla birlikte), db `METRICS_ENABLE` (`conf/db.txt`,
  yoksa 1). Kapalıyken satır yazılmaz; sayaçlar (birkaç atomik) her zaman çalışır.
- Yazma: kendi kuyruğu ve worker'ı (`metrics_writer`); sağlık satırının kuyruğunu, `metrics_dropped` sayacını ya da
  syslog havuzunu paylaşmaz.

**Satırlar.** Önek `schema=1 src=sql kind=sum|conn host=<host|db> pid=<pid> uptime_s=<n>`.
- `kind=sum`: süreç başına **her 10 sn**; değerler **o pencerenin Δ'sı** (`window_ms` gerçek süre, `first=1` ilk
  pencere = süreç başından beri) ve anlık en kötü değerler.
- `kind=conn`: bağlantı başına, **sadece** gerekince: `reason=start` (ilk pencere), `anomaly` (o pencerede hata, tekrar,
  yeniden bağlanma görüldü, takılma, en eski bekleyen ≥ 1 sn ya da worker yok), `periodic` (5 dk'da bir), `final`
  (kapanış). Sayaçlar **kümülatif** (`*_total`, süreç başından beri): iki satır arasındaki fark, aradaki satırlar yazılmasa
  ya da düşse bile doğrudur. Süreç yeniden başlarsa `pid` değişir ve toplamlar 0'dan başlar.

**Bağlantı etiketleri** (`kind=conn`): `owner` (bağlantıyı tutan nesne), `target` (hangi config veritabanı), `role`.
| owner | target | role | Not |
|---|---|---|---|
| `dbmanager` | `player` (auth çekirdeğinde `account`) | `main`, `direct` | `game/config.cpp:902-906` |
| `accountdb` | `common` | `main`, `direct` | adı yanıltıcı: `common_sql`'e bağlanır (`game/config.cpp:854,900`) |
| `logmanager` | `log` | `main` | auth çekirdeğinde kurulmaz → satırı yok |
| `db` | `player`, `account`, `common`, `hotbackup` | `main` (ReturnQuery), `async`, `direct` | bağlanmayan slot yok sayılır |
`mode=thread` worker'lı (Async/ReturnQuery), `mode=direct` çağıranın thread'inde (`DirectQuery`, game döngüsünü bekletir).

| Alan (`conn`) / Δ adı (`sum`) | Tür | Anlam |
|---|---|---|
| `q`, `cq`, `rq` | anlık | ana kuyrukta / worker'ın kopya kuyruğunda (takılı baş dahil) / sonuç kuyruğunda bekleyen |
| `oldest_ms` (`sum`: `oldest_ms_max`) | anlık | bitmemiş en eski sorgu ne zamandır bekliyor (kuyruğa girişten; ±10 ms) |
| `stuck_ms` (`sum`: `stuck_conns`) | anlık | baştaki sorgu ne zamandır başarısız olup tekrar bekliyor |
| `worker` (`sum`: `workers_down`) | anlık | worker thread çalışıyor mu (ilk bağlantısı kurulamadıysa 0 ve kuyruk büyür) |
| `pushed_total` / `pushed` | kümülatif / Δ | kuyruğa giren |
| `ok_total` / `ok` | kümülatif / Δ | son denemesi hatasız biten |
| `err_total` / `err` | kümülatif / Δ | son denemesi hatayla biten: **o ifade uygulanmadı** (veri kaybı kanıtı değil) |
| `retry_total` / `retry` | kümülatif / Δ | başarısız olup kuyrukta tekrar bekleyen deneme (tekrarın **ne zaman** yapılacağı garanti değil, 1a S1) |
| `reconnect_seen_total` / `reconnect_seen` | kümülatif / Δ | sorgudan önce bağlantının yenilendiği **fark edildi** (Connector/C'nin sessiz yeniden bağlanması dahil; kopma anı değil) |
| `e2006 e2013 e2014 e1205 e1213 e_other` (+`_total`) | kümülatif / Δ | başarısız **denemeler** hata koduna göre; toplam = `err + retry` (+ `direct_err`) |
| `exec_n_total`, `exec_us_total`, `exec_max_us` / `exec_n`, `exec_us`, `exec_max_us` | kümülatif, aralık / Δ | biten sorguların son denemesinin süresi: kilit bekleme + çalışma + ağ, **ayrılamaz** (`conn`'da `exec_max_us` önceki `conn` satırından beri) |
| `direct_n`, `direct_err`, `direct_max_ms` (`sum`); `exec_ms_total`, `exec_max_ms` (`direct` `conn`) | Δ / kümülatif | `DirectQuery` sayısı/hatası/en uzunu; ucuz saat, ±10 ms: kısa sorguları ölçmez, döngüyü bekleten uzunları yakalar |
| `unexecuted_at_quit` | sayı | sadece db'nin `reason=final` satırında: worker durduğunda kopya kuyruğunda kalıp **hiç çalıştırılmayan**. game'de yok (bağlantıları log kapandıktan sonra kapanıyor) |
| `metrics_dropped`, `metrics_write_errors` (`sum`) | süreç başından beri | bu akışın kendi kayıpları |
| `build`, `build_dirty`, `build_src` (`sum`) | sabit | sağlık satırıyla aynı kanonik derleme kimliği (yukarıda) |

**db'ye özel (`kind=sum`, Δ):** `save_player_ok/_err`, `save_item_ok/_err`, `destroy_item_ok/_err`, `save_quest_ok/_err`,
`save_safebox_ok/_err`, `award_taken_ok/_err`. `CClientManager::AnalyzeQueryResult` girişinde, peer aranmadan önce
sayılır (cache flush kayıtları `dwIdent=0` ile gelir ve peer'siz erken dönüşe düşer; sayım onlardan önce).
- `_err` = **son deneme hatalı, o SAVE uygulanmadı**. Kayıtlar tam satır yazar; aynı satırın sonraki başarılı SAVE'i bunu
  düzeltir. Kalıcı kayıp ancak ondan önce önbellekten düşerse ya da süreç kapanırsa olur; sayaç bunu **kanıtlamaz**.
- Kapanıştaki cache flush'ın sonuçları hiç işlenmez (ana döngü bitmiştir): bu alanlarda yoktur; sadece `player/main`
  bağlantısının `err_total`'ında ve `unexecuted_at_quit`'te görünür.

**Okurken:** `exec_*` uzun ama `e1205=0` → Aria/MyISAM tablo kilidi beklemesine **işaret** (kanıt değil); dbstat'ın
`table_locks_waited`'ı ile birlikte oku. `retry>0` ve `stuck_ms` büyüyorsa o bağlantının kuyruğu tıkalı (1a S3);
arkasındaki bütün sorgular bekliyor (`cq`, `oldest_ms`).

# MariaDB / OS (dbstat)

Ayrı, salt okunur bir toplayıcı: `deploy/freebsd/metrics/m2dev-dbstat/` (tasarım ve ölçümler:
`docs/engineering/db-step1b-collector.md`). game, db ve MariaDB ona bağımlı değil; durursa sadece bu satırlar durur.
Dosya: `/var/log/m2dev-metrics/dbstat_YYYY-MM-DD.log`, 14 gün. Aralık 10 sn. Satır başı yukarıdakiyle aynı biçim,
`schema=1 src=dbstat kind=db|os|proc`. Alanlar adıyla okunur; yeni alan eklenebilir, anlam değişirse `schema` artar.

**Değer türleri:** **Δ** = önceki başarılı örnekten bu yana fark; **anlık** = örnek anı. Özel değerler: `NA` = bu
MariaDB/FreeBSD sürümünde yok (toplayıcı çalışmaya devam eder, `na` sayısı artar); `-` = bu pencerede güvenilir Δ yok
(ilk örnek, yeniden başlatma, sayaç geri gitti). Ham sayaç yazılmaz.

**`kind=db`:** `up` (0 ise sadece `err=connect|auth|gone|lost|other` ve `retry_in_s`), `version`, `long_query_time_s`,
`uptime_s`, `window_s` (Uptime farkı), `first` (toplayıcının ilk örneği), `restart` (Uptime geri gitti → MariaDB yeniden
başladı, bütün Δ `-`), `reset` (yeniden başlatma olmadan geri giden sayaç sayısı; o alan `-`), `na`, `write_errors`
(toplayıcı başından beri yazılamayan satır).
| Alan | Tür | MariaDB | Not |
|---|---|---|---|
| `questions`, `com_select/insert/update/replace/delete` | Δ | `Questions`, `Com_*` | `INSERT ... SELECT` `com_insert`'e girmez (`Com_insert_select`) |
| `row_lock_waits`, `row_lock_time_ms` | Δ | `Innodb_row_lock_waits/time` | InnoDB satır kilidi bekleme sayısı ve toplam süresi (ms) |
| `row_lock_current_waits` | anlık | `Innodb_row_lock_current_waits` | |
| `deadlocks` | Δ | `Innodb_deadlocks` | |
| `table_locks_waited`, `table_locks_immediate` | Δ | `Table_locks_*` | Aria/MyISAM tablo kilidi. **Sadece sayı; süre yok** — buradan süre/performans türetilmez (süre: istemci tarafı 1c). Tablo sonuna `INSERT` okuma sırasında beklemez (concurrent insert), sayılmaz |
| `innodb_fsyncs`, `innodb_log_bytes`, `aria_log_syncs` | Δ | `Innodb_data_fsyncs`, `Innodb_os_log_written`, `Aria_transaction_log_syncs` | motor başına disk senkron maliyeti |
| `bp_read_requests`, `bp_reads` / `aria_cache_read_requests`, `aria_cache_reads` | Δ | buffer pool / Aria pagecache | önbellek isabeti = 1 − reads/requests |
| `history_list_length`, `bp_dirty_pages` | anlık | `Innodb_*` | |
| `threads_running`, `threads_connected` | anlık | `Threads_*` | toplayıcının kendi bağlantısı ve sorgusu dahil |
| `aborted_clients`, `aborted_connects`, `conn_errors_max` | Δ | `Aborted_*`, `Connection_errors_max_connections` | |
| `slow_queries` | Δ | `Slow_queries` | sadece `long_query_time_s` üstü; **0 "yavaş sorgu yok" demek değildir** (bugün eşik 10 sn) |

**`kind=os`:** `load1`, `mem_free_mb`, `swap_used_mb` (anlık); `disk` (MariaDB datadir'inin diski, otomatik; ZFS'te
`--disk`) ve pencere ortalaması `r_s w_s mb_r_s mb_w_s ms_r ms_w qlen busy_pct` (devstat; ilk pencere `-`).

**`kind=proc`:** her izlenen süreç için `name` (argv[0]: `mariadbd`, `db`, `game_auth`, `channelN_coreM`,
`m2dev-dbstat`), `pid`, `first` (pid ilk kez görüldü: başlangıç ya da yeniden başlatma), `restart` (aynı pid, çalışma
süresi geri gitti), `cpu_us` (Δ user+sys, µs), `rss_kb`. game sağlık kaydıyla `pid` + zaman üzerinden birleştirilir.

**Erişim ve arıza:** MariaDB'ye unix socket, `USAGE` yetkili kullanıcı (yazamaz, veri okuyamaz, başkalarının
sorgularını göremez); sadece `SHOW GLOBAL STATUS WHERE Variable_name IN (...)` ve bağlanınca/10 dk'da bir
`SELECT VERSION(), @@long_query_time, @@datadir`. Bağlantı yoksa yeniden deneme aralığı 10 → 20 → 40 → 60 sn (en fazla).
Çökerse servis en erken 30 sn sonra yeniden başlatır. Disk dolarsa satır atılır, `write_errors` artar.

**Okuma:** `python3 tools/metrics/m2metrics.py --dbstat /var/log/m2dev-metrics --hours 24` (özet: sorgu hızı, kilit
beklemeleri, senkron, önbellek isabeti, OS, süreç başına CPU/RSS); `--raw` satırları aynen basar.

**Maliyet (test VM'de ölçüldü):** toplayıcı 0,65–1,14 ms CPU/örnek (iki koşu; 10 sn aralıkta ≤ %0,012 çekirdek), RSS ~10,7 MB sabit;
MariaDB tarafı ~0,45 ms/okuma; çıktı ~18 MB/gün. Toplayıcı kendi maliyetini `name=m2dev-dbstat` satırında raporlar.

# Telemetri sözleşmesi (bütün kaynaklar)

Panel, yönetim servisi ve teşhis yapan agent'lar bu kaynakları **olduğu gibi** okur (`docs/architecture.md` →
"Kontrol katmanı ilkeleri"). Yeni bir telemetri kaynağı ya da alanı bu kurallara uyar:
- **Biçim:** tek satır, `<yerel zaman %Y-%m-%dT%H:%M:%S%z> anahtar=değer ...`; değerde boşluk ve `=` yok. Alanlar **adıyla**
  okunur, sırayla değil. Yeni alan eklenebilir; bir alanın anlamı ya da biçimi değişirse `schema` artar.
- **UTC farkının iki yazımı:** kaynaklar aynı farkı iki biçimde yazar ve okuyucular **ikisini de** kabul etmelidir
  (2026-10-06'da gerçek dosyalarda doğrulandı; başka biçim görülmedi):
  - `+03:00` — spdlog desenindeki `%z`: game sağlık satırı ve SQL satırları (`metrics_*.log`, `sql_*.log`;
    `common/metrics_writer.h`).
  - `+0300` — C `strftime`/`date` `%z`: dbstat satırı ve `time=` alanı olan durum dosyaları (`status-backup-*`,
    `status-restore-test`, yedek makinesinin `status-pull`/`status-daily`'si).
  Aynı anı gösterirler; Python `datetime.strptime(..., "%Y-%m-%dT%H:%M:%S%z")` ikisini de ayrıştırır (`m2metrics.py`).
  Biçimleri eşitlemek için çalışan kod değiştirilmez.
- **Kaynak:** `src=` alanı ya da dosya öneki (`metrics_` game sağlık satırı — `src` alanı yok, dosya önekinden anlaşılır;
  `sql_` (`src=sql`); `dbstat_`; `status-*` yedek/restore-test). Süreç başına dosyalar sürecin `log/` klasöründe, günlük, 14 gün.
- **Değer türleri:** Δ (pencere farkı), kümülatif toplam (`*_total`, süreç başından beri) ve anlık ayrı adlandırılır ya da
  tabloda belirtilir. `NA` = bu sürümde yok, `-` = bu pencerede güvenilir fark yok. Yeniden başlatma `pid` değişimi ya da
  `restart=1` ile görünür; sahte sıçrama yazılmaz.
- **`host` alanı:** game satırlarında **süreç adı** (`channel1_1`, `auth`…), dbstat'ta **makine adı**. Değiştirilmedi
  (game satırı yayında); çok makinede toplayan servis makine bilgisini dosyayı nereden okuduğundan ekler.
- **Hassas veri yok:** şifre, gizli bilgi, IP, hesap/karakter adı, SQL metni yazılmaz.
- **Gözlem gözleneni bozmaz:** yazan süreç okuyucuyu beklemez, okuyucuya bağlanmaz; yazma hatası satırı düşürür ve sayar.
- **Kendi kaybını raporlar:** `metrics_dropped`, `metrics_write_errors`, `write_errors`.
- **Derleme kimliği:** game sağlık ve SQL `sum` satırları aynı `build= build_dirty= build_src=` alanlarını taşır (satır
  başına +79 bayt, ölçüldü); `m2metrics.py` süreç başına görülen derlemeyi ve dönem içindeki değişikliği gösterir. Bu bir
  **beyandır** (binary'nin gömülü işareti), özgünlük kanıtı değildir; tam dosya kanıtı `deploy.log`'daki SHA-256
  (`docs/build-and-run.md` → "Kanıtın sınırı"). dbstat ayrı derlenir, henüz kimlik taşımaz.

**Bilinen teşhis açıkları** (`docs/roadmap.md`): T-1 `syserr.log` yeniden başlatmada sıfırlanıyor; T-2 (1.9) derleme
kimliği test VM'de çalışan süreçlerde doğrulandı, production temiz-git derleme yolu henüz yok; T-3 disk boş
alanı ölçülmüyor; T-4 `service m2dev status` tek süreç canlıyken sağlıklı görünüyor.

# Olay teşhisi (insan ve agent)

Bir sorun bildirildiğinde kodu taramadan önce **bu sırayla** salt okunur kaynaklara bak; hiçbiri veri değiştirmez.
**Bugünkü okuma yetkileri** (test VM, 2026-10-06, yetkisiz kullanıcıyla denendi): `ssh bsd` **root** olarak girer, bütün
kaynakları o okur. Root olmayan bir kullanıcı game süreçlerinin `log/` dosyalarını, `syserr.log`'u ve `pids.json`'u okuyabilir
(`644`); dbstat'ı (`/var/log/m2dev-metrics`, `0750 m2stat:wheel`) ve yedek durum dosyalarını (`/var/backups/m2dev`, `0700`,
dosyalar `0600 root`) **okuyamaz**. Root gerektirmeyen ortak salt-okuma grubu Faz 3 işi (`docs/roadmap.md` 3.1).

| Soru | Kaynak | Komut / yer |
|---|---|---|
| Hangi süreçler çalışıyor, yeniden başladı mı? | dbstat `kind=proc` (`first=1`, `pid` değişimi), game satırlarında `pid`/`uptime_s` | `m2metrics.py --dbstat … --hours 1`; `pids.json`. `service m2dev status`'a güvenme (T-4) |
| Lag var mı, hangi çekirdekte? | game sağlık satırı: `late_pulses`, `iter_gap_max_us`, `work_max_us`, bölüm payları | `m2metrics.py --hours 1` |
| Döngü mü, işletim sistemi mi? | `iter_gap_max_us` büyük ama `work_max_us` küçük → süreç çalışamadı (CPU/swap/VM); dbstat `load1`, `mem_free_mb`, `swap_used_mb` | yukarıdaki iki özet |
| MariaDB mi, disk mi? | dbstat: `row_lock_waits/time_ms`, `deadlocks`, `table_locks_waited`, `threads_running`, `ms_w`, `qlen`, `busy_pct`, `mariadbd` CPU | `m2metrics.py --dbstat` (root ya da `m2stat`/`wheel`) |
| SQL kuyruğu mu? | `log/sql_*.log`: `kind=sum` (kuyruk, en eski bekleyen, takılma, hata/tekrar, db'de SAVE sonuçları), anormallikte `kind=conn` | `m2metrics.py --sql --hours 1`; "SQL (sql_*.log)" bölümü |
| Yedek mi? | `status-backup-hot` (`lock_ms`, `duration_s`, zaman); dakika :17'de çalışır | `/var/backups/m2dev/status-*` (root) |
| Son yedek/geri yükleme testi sağlam mı? | `status-backup-*`, `status-restore-test` (`result`, zaman → yaş), yedek makinesinde `status-pull`/`status-daily` | `docs/backup.md` |
| Hata kaydı | `syserr.log` (sadece **son açılıştan beri**, T-1), `log/syslog_*.log` | süreç klasörü |
| Hangi binary? | Satırdaki `build=`; sürecin `version.txt`/`VERSION.txt`'i; kurulu dosya ↔ kimlik eşlemesi `/var/db/m2dev/deploy.log` ve `share/bin/BUILD` (SHA-256). Kimlikten önceki binary'lerde alan yok | `m2metrics.py` (süreç başına "build …") |

Teşhis ayrımı: **game döngüsü** (bir çekirdekte `late_pulses` + yüksek `work_max_us`) · **işletim sistemi/VM** (gap büyük,
work küçük, bütün çekirdeklerde aynı anda) · **MariaDB** (kilit beklemeleri, `threads_running`) · **disk** (`ms_w`, `qlen`,
`busy_pct`) · **SQL kuyruğu** (1c) · **yedek** (aynı dakikada `lock_ms` ve `table_locks_waited`).
