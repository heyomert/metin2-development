# DB adım 1b: MariaDB + işletim sistemi toplayıcısı — etki analizi ve tasarım

**Durum: TASLAK, onay bekliyor (2026-10-06).** Kod, kurulum, servis, commit yok. Ölçümler salt okunur (canlı test sunucusunda
`SHOW STATUS`, `ps`/`sysctl`) ya da geçici MariaDB'de yapıldı. Üst plan: `db-standard.md`, `db-step1-measurement.md`.

**Amaç:** InnoDB dönüşümü ve AsyncSQL düzeltmesinden **önce** bugünkü karma motor yapısının (Aria/MyISAM/InnoDB) DB ve
işletim sistemi tarafındaki ölçülebilir baseline'ı; aynı alanlarla sonra A/B. Az ama karar verdirecek sinyal.

## 0. Ölçülerek doğrulanan olgular (MariaDB 11.8.9, FreeBSD 15.1, test VM)
| Konu | Sonuç | Nasıl |
|---|---|---|
| InnoDB satır kilidi | 2 sn'lik bekleme → `Innodb_row_lock_waits` +1, `Innodb_row_lock_time` +1703 (ms), `_max` 1703 | geçici sunucu, `FOR UPDATE` + ikinci oturum |
| Aria/MyISAM tablo kilidi | 3 sn'lik okuma sırasında `UPDATE` 2.522 ms bekledi → `Table_locks_waited` **+1** (sadece **sayı**, süre yok); işlem listesinde `Waiting for table level lock` | geçici sunucu, iki motor ayrı |
| Aria/MyISAM `INSERT` | okuma sırasında tablo sonuna `INSERT` **beklemedi** (concurrent insert) → sayaç artmadı | aynı |
| `Slow_queries` | `long_query_time=10` (bugünkü değer) ile 1 sn'lik sorgu sayılmadı; oturumda 0,5 yapılınca sayıldı; `slow_query_log=0` sayacı etkilemiyor | geçici sunucu |
| `FLUSH STATUS` | denenen global sayaçlar (`Table_locks_*`, `Innodb_row_lock_*`, `Slow_queries`, `Questions`) **sıfırlanmadı** | geçici sunucu |
| `Innodb_row_lock_time_max` | başlangıçtan beri en büyük; pencere bazında anlamsız, sıfırlanamıyor | yukarıdaki iki satır |
| Mevcut alanlar | istenen 33 adın **33'ü** var; olmayan bir ad istenince **hata yok, satır gelmiyor** | `SHOW GLOBAL STATUS WHERE Variable_name IN (...)` |
| Yetki | `USAGE` (unix_socket) kullanıcı bütün global sayaçları okuyor; `INSERT`/`SELECT` reddedildi; işlem listesinde sadece kendini görüyor | geçici sunucu, OS kullanıcısı `nobody` |
| Disk | `iostat -x` → `r/s w/s kr/s kw/s ms/r ms/w ms/o ms/t qlen %b` (gecikme ve kuyruk var); MariaDB datadir `/dev/ada0p2` | VM |

**Maliyet (canlı test sunucusu, 37 bağlantı, salt okunur):**
| Okuma | Duvar | İstemci CPU | MariaDB CPU |
|---|---|---|---|
| durum sorgusu (33 ad), kalıcı oturum | 0,80 ms | 0,12 ms | **0,45 ms** |
| işlem listesi sayımı, kalıcı oturum | 0,97 ms | 0,09 ms | 0,62 ms |
| durum sorgusu, her seferinde yeni istemci | 23 ms | 18,6 ms | **5,8 ms** (bağlantı kurma + kimlik doğrulama) |
| `ps -axo pid,rss,cputime,args` / `procstat -r` (8 pid) / `sysctl` | 3,0 / 2,4 / 1,4 ms | 2,6 / 2,0 / 1,2 ms | 0 |

10 sn'de bir kalıcı oturumla MariaDB'ye düşen yük bir çekirdeğin ~%0,005'i; her seferinde yeni istemci ~%0,06 (13 kat).
Not: VM saat okuması pahalı (`clock-cost`); fiziksel sunucuda daha düşük olması beklenir.

## 1. Toplanacak alanlar
Satırlar `docs/monitoring.md` sözleşmesinde: zaman damgası + `anahtar=değer`, `schema`, alanlar adıyla okunur. `src=dbstat`.
**Δ** = önceki başarılı örnekten bu yana fark (pencere); **anlık** = örnek anındaki değer.

### `kind=db` (MariaDB)
| Alan | Tür | Kaynak (`SHOW GLOBAL STATUS` adı) | Neden |
|---|---|---|---|
| `up` | anlık | bağlantı sonucu | MariaDB erişilebilir mi |
| `version`, `long_query_time_s` | anlık | `VERSION()`, `@@long_query_time` | A/B karşılaştırmada ortam kimliği; `slow_queries`'in duyarlılığı |
| `uptime_s`, `window_s` | anlık | `Uptime`, ardışık farkı | yeniden başlatma tespiti, pencere süresi |
| `first`, `restart`, `reset`, `na` | anlık | toplayıcı | Δ'ların neden yok olduğu (bölüm 4) |
| `questions` | Δ | `Questions` | iş hacmi; diğerlerini normalize etmek için |
| `com_select/insert/update/replace/delete` | Δ | `Com_*` | yazma karışımı (`db` önbelleği ağırlıkla `REPLACE`) |
| `row_lock_waits`, `row_lock_time_ms` | Δ | `Innodb_row_lock_waits`, `Innodb_row_lock_time` | InnoDB bekleme sayısı ve **süresi**; dönüşümün ana A/B sinyali |
| `row_lock_current_waits` | anlık | `Innodb_row_lock_current_waits` | şu an bekleyen |
| `deadlocks` | Δ | `Innodb_deadlocks` | 1c'deki 1213 sayısıyla çapraz kontrol |
| `table_locks_waited`, `table_locks_immediate` | Δ | `Table_locks_*` | Aria/MyISAM tablo kilidi bekleme **sayısı** (süre yok, bkz. 9) |
| `innodb_fsyncs`, `innodb_log_bytes`, `aria_log_syncs` | Δ | `Innodb_data_fsyncs`, `Innodb_os_log_written`, `Aria_transaction_log_syncs` | motor başına dayanıklılık (disk senkron) maliyeti; dönüşümde değişecek |
| `bp_read_requests`, `bp_reads` | Δ | `Innodb_buffer_pool_read_requests/reads` | InnoDB önbellek isabeti → buffer pool boyutu kararı (bugün 128 MB) |
| `aria_cache_read_requests`, `aria_cache_reads` | Δ | `Aria_pagecache_read_requests/reads` | Aria önbellek isabeti (dönüşüm öncesi karşılığı) |
| `history_list_length`, `bp_dirty_pages` | anlık | `Innodb_history_list_length`, `Innodb_buffer_pool_pages_dirty` | InnoDB temizleme gecikmesi ve yazılmamış sayfa birikimi (yük altında) |
| `threads_running`, `threads_connected` | anlık | `Threads_*` | eşzamanlı çalışan sorgu, bağlantı sayısı |
| `aborted_clients`, `aborted_connects`, `conn_errors_max` | Δ | `Aborted_*`, `Connection_errors_max_connections` | bağlantı sağlığı (kopma, reddedilme) |
| `slow_queries` | Δ | `Slow_queries` | sadece `long_query_time` üstü; bugün 10 sn → neredeyse kör, değeri değiştirmek ayrı karar |

**Bilerek alınmayanlar:** `Innodb_row_lock_time_max` (pencere bazında anlamsız), MyISAM `Key_*` (uygulama MyISAM'ı sadece
log'da), `Created_tmp_disk_tables` (karar sinyali değil), işlem listesi ve `performance_schema` (bkz. 4, 8).

### `kind=os`
`load1`, `mem_free_mb`, `swap_used_mb` (anlık); datadir'in diski için `r_s w_s mb_r_s mb_w_s ms_r ms_w qlen busy_pct`
(pencere ortalaması). Neden: aynı makinede game/db/MariaDB rekabeti, bellek baskısı (VM daha önce host belleği yüzünden
askıya alındı), disk gecikmesi ve kuyruğu (dönüşümün fsync etkisi).

### `kind=proc` (her süreç için bir satır)
`name` (argv[0]'ın adı: `mariadbd`, `db`, `game_auth`, `channel1_core1`…), `pid`, `cpu_us` (Δ user+sys, µs), `rss_kb`
(anlık), `restart`. Neden: kaynak rekabetini süreç bazında görmek; game sağlık kaydıyla `pid` + zaman ile birleştirilir.

## 2. Kaynak ve sürüm bağımlılığı
- MariaDB: sadece `SHOW GLOBAL STATUS WHERE Variable_name IN (...)` + `VERSION()` + `@@long_query_time`. Ad listesi
  sabit; sunucuda olmayan ad **satır döndürmüyor** (ölçüldü) → alan `NA`, `na` sayısı artar; toplayıcı durmaz. Hiçbir
  alan `performance_schema`'ya, işlem listesine ya da sürüme özgü tabloya bağlı değil.
- FreeBSD: yük/bellek/takas `sysctl`; süreçler `kvm_getprocs` (ya da `ps`); disk `devstat` (ya da sürekli açık
  `iostat -x -w 10`). Linux'a taşınırsa bu katman uyarlanır (`/proc`); DB katmanı aynı kalır.

## 3. Sorgulama yöntemi (öneri) ve maliyeti
**Ayrı bir küçük daemon (C++, mevcut derleme araç zinciri ve gömülü Connector/C ile; `tools/sql-reliability` gibi
`server-src` CMake'ine dokunmadan derlenir).** Gerekçe ölçümde:
- Kalıcı bağlantı → MariaDB'ye 0,45 ms/10 sn; her seferinde yeni istemci 13 kat (5,8 ms) + bağlantı churn'ü.
- `MYSQL_OPT_CONNECT_TIMEOUT`/`READ_TIMEOUT` ile asılı kalma yok (sh'ta kalıcı `mariadb` oturumunu FIFO ile beslemek
  zaman aşımı ve yeniden bağlanma için kırılgan).
- OS değerleri `sysctl(3)`/`kvm_getprocs(3)`/`devstat(3)` ile süreç başlatmadan okunur (ölçülen `ps`+`sysctl` ~4 ms/10 sn de
  kabul edilebilir; yerli API daha da ucuz).
- **Alternatif (daha basit, ölçülmüş):** sh + her 10 sn'de yeni `mariadb` + `ps` + `sysctl` + sürekli `iostat` →
  toplam ~%0,25 çekirdek (istemci) + %0,06 (MariaDB). Kabul edilebilir ama 13 kat sunucu maliyeti ve zayıf zaman aşımı
  kontrolü. **Karar (2026-10-06): C++ kalıcı toplayıcı** — `deploy/freebsd/metrics/m2dev-dbstat/`.
- Toplayıcı CPU/RSS'i de kendi `kind=proc` satırında raporlanır (kendi maliyeti görünür).

## 4. Arıza davranışı (fail-open: toplayıcı ölür, ölçüm eksik kalır, başka hiçbir şey etkilenmez)
- **game/db bağımlılığı yok:** onlar toplayıcının dosyalarını okumaz, ona bağlanmaz; toplayıcı onlara sinyal göndermez.
- **MariaDB kapalı / yetki kaybı:** `kind=db up=0 err=<sınıf> retry_in_s=N`, Δ alanları yok. Sınıflar: `auth`
  (1045/1698), `connect` (2002/2003), `gone` (2006), `lost` (2013, okuma zaman aşımı dahil), `other`. Yeniden bağlanma
  üstel geri çekilme: aralık → ×2 → en fazla 60 sn (10 sn aralıkta 10 → 20 → 40 → 60), asla aralıktan sık değil →
  yeniden deneme fırtınası olamaz (ölçüldü: 2 sn aralıkta 2 → 4 → 8 sn, T3). Otomatik yeniden bağlanma kapalı.
- **Sorgu asılı kalırsa:** bağlantı 3 sn, okuma/yazma 5 sn zaman aşımı → o örnek `up=0 err=lost`, bağlantı kapatılıp geri
  çekilmeyle açılır.
- **Eksik alan:** `NA` + `na` sayısı; sözleşme sabit kalır.
- **Disk dolu / yazma hatası:** satır atılır ve sayılır (`write_errors`, game sağlık kaydıyla aynı ilke), bellekte birikme yok.
- **Toplayıcı çökerse:** `daemon(8)` gecikmeli yeniden başlatır (`-r -R 30`); ilk örnek `first=1`.
- **Salt okunur:** MariaDB kullanıcısı `USAGE` (unix_socket, ayrı OS kullanıcısı); yazma/okuma/şema/kilit imkânı yok
  (ölçüldü: `INSERT`/`SELECT` reddedildi). `PROCESS` **verilmez**: işlem listesi diğer oturumların SQL metnini gösterir;
  bu yüzden "şu an tablo kilidi bekleyen" sayısı 1b'de yok (1c istemci tarafı süreleri kapatır).
- **Hassas veri yok:** sadece sayaçlar ve süreç adları (argv[0] adı; argümanlar alınmaz). SQL metni, hesap/karakter adı, IP
  yok.

## 5. Δ, yeniden başlatma ve sıfırlanma kuralları
- Ham değer **yazılmaz**; sadece Δ ve anlık. Δ = mevcut − önceki, **sadece** şu durumda: önceki örnek var, `Uptime`
  büyüdü, sayaç küçülmedi.
- `Uptime` küçüldü → `restart=1`, bütün Δ'lar yok (sahte sıçrama yok). İlk örnek → `first=1`, Δ yok.
- Tek bir sayaç küçüldü (`FLUSH STATUS` bazı sayaçları sıfırlar; denenenler sıfırlanmadı) → o alan yok, `reset` +1.
- Taşma: sayaçlar 64 bit; pratikte erişilmez (taşma da "küçüldü" kuralına düşer).
- Süreç: `pid` değişti ya da yeni → `restart=1`, `cpu_us` yok.
- `window_s` gerçek süre (`Uptime` farkı); oranlar (/sn) okuyucu tarafında hesaplanır.

## 6. Çıktı örneği
```
2026-10-06T12:00:10+0300 schema=1 src=dbstat kind=db host=m2dev-acceptance up=1 version=11.8.9-MariaDB long_query_time_s=10 uptime_s=14271 window_s=10 first=0 restart=0 reset=0 na=0 questions=312 com_select=120 com_insert=8 com_update=3 com_replace=180 com_delete=0 row_lock_waits=0 row_lock_time_ms=0 row_lock_current_waits=0 deadlocks=0 table_locks_waited=0 table_locks_immediate=150 innodb_fsyncs=4 innodb_log_bytes=10240 aria_log_syncs=12 bp_read_requests=900 bp_reads=0 aria_cache_read_requests=400 aria_cache_reads=0 history_list_length=5 bp_dirty_pages=2 threads_running=1 threads_connected=37 aborted_clients=0 aborted_connects=0 conn_errors_max=0 slow_queries=0 write_errors=0
2026-10-06T12:00:10+0300 schema=1 src=dbstat kind=os host=m2dev-acceptance load1=0.42 mem_free_mb=3100 swap_used_mb=0 disk=ada0 r_s=0.1 w_s=12.3 mb_r_s=0.000 mb_w_s=0.176 ms_r=0.4 ms_w=1.1 qlen=0 busy_pct=1
2026-10-06T12:00:10+0300 schema=1 src=dbstat kind=proc host=m2dev-acceptance name=channel1_core1 pid=11942 first=0 restart=0 cpu_us=160412 rss_kb=612000
```

## 7. Hacim, saklama
Ölçüldü (entegrasyon testi T7, 300 örnek, 9 `proc` satırı): 656.646 bayt → örnek başına ~2,2 KB → 10 sn aralıkta
**~18 MB/gün**, 14 gün **~250 MB**. Tahmin (1,8 KB) düşük kalmıştı: `db` satırı ~1 KB. Günlük dosya + 14 gün (game sağlık kaydıyla aynı kural); yer: `/var/log/m2dev-metrics/dbstat_YYYY-MM-DD.log`.

## 8. Kurulum ve geri alma (ayrı onayla, VM'de)
Kurulum: OS kullanıcısı `m2stat` (nologin) + MariaDB `m2stat@localhost IDENTIFIED VIA unix_socket` (`USAGE`); binary
`/usr/local/sbin/m2dev-dbstat`; rc servis (`daemon -f -S -r -R 30 -u m2stat`: supervisor root, toplayıcı `m2stat`);
çıktı klasörü; `m2metrics.py` okuyucusu. Adımlar (test VM'de 2026-10-06 uygulandı, önce `m2dev-backup hot`):
```sh
pw useradd m2stat -d /nonexistent -s /usr/sbin/nologin -c "m2dev dbstat collector"
mariadb -e "CREATE USER m2stat@localhost IDENTIFIED VIA unix_socket"   # sadece USAGE
sh deploy/freebsd/metrics/m2dev-dbstat/build.sh /tmp/dbstat-build      # derler + birim testleri
install -o root -g wheel -m 0555 /tmp/dbstat-build/m2dev-dbstat /usr/local/sbin/m2dev-dbstat
install -o root -g wheel -m 0555 deploy/freebsd/metrics/m2dev-dbstat/m2dev_dbstat.rc /usr/local/etc/rc.d/m2dev_dbstat
sysrc m2dev_dbstat_enable=YES && service m2dev_dbstat start
```
Geri alma: servisi durdur/devre dışı bırak, dosyaları sil, `DROP USER m2stat@localhost`, `pw userdel m2stat`. game/db/
şema/yapılandırma değişmez. **Doğrulandı:** `kvm_getprocs` ile root'a ait süreçlerin (`mariadbd`, `channel1_core1`)
`ki_runtime`/`ki_rssize`'ı ve `kvm_getargv` adı ayrıcalıksız kullanıcıyla (`nobody`) okunuyor
(`security.bsd.see_other_uids=1` varsayılan; entegrasyon testi T1). Bu sysctl kapatılırsa diğer kullanıcıların süreçleri
listeden düşer → o süreçlerin `kind=proc` satırı gelmez (sessizce yanlış değer yerine eksik satır).

## 9. 1c ile ilişki (çakışma yok, bilinçli çapraz kontrol var)
| Konu | 1b (sunucu tarafı, toplam) | 1c (istemci tarafı, bağlantı başına) |
|---|---|---|
| kilitlenme | `deadlocks` Δ | `e1213` (hangi bağlantı/sorgu sınıfı) → çapraz kontrol |
| iş hacmi | `questions`, `com_*` | `ok/err` sorgu sınıfına göre → çapraz kontrol |
| Aria/MyISAM bekleme | sadece **sayı** (`table_locks_waited`) | **süre** (sorgu süresi; `PROCESS` gerekmeden) |
| InnoDB bekleme | sayı + toplam süre | sorgu bazında süre |
| kuyruk / tıkanma / retry / atılan | yok (sunucu bilemez) | `q`, `cq`, `oldest_ms`, `stuck_ms`, `retry`, `drop` |
| süreç CPU/RSS, disk, bellek | var | yok |
Tekrar eden ölçüm yok; aynı olayın iki taraftan görünümü A/B ve tutarlılık kontrolü için kullanılır.

## 10. Test planı ve kabul kriterleri
- **Doğruluk:** geçici sunucuda bilinen olay üret → doğru alanda doğru Δ: N satır kilidi beklemesi (`row_lock_waits=N`,
  `row_lock_time_ms` ≈ toplam), M Aria/MyISAM tablo kilidi beklemesi (`table_locks_waited=M`), bir kilitlenme
  (`deadlocks=1`), K yazma (`com_*`).
- **Yeniden başlatma:** geçici MariaDB'yi yeniden başlat → `restart=1`, sahte sıçrama yok; toplayıcıyı yeniden başlat →
  `first=1`; izlenen süreç yeniden başlarsa `restart=1`.
- **Arıza:** MariaDB kapalı → `up=0`, bağlantı denemeleri 10/20/40/60 sn aralığıyla (sunucu `Connections` ya da zaman
  damgalarıyla sayılır), fırtına yok; yetki kaldırma → `up=0 err=auth`; sürümde olmayan ad → `NA`; küçük tmpfs'te disk
  dolu → `write_errors` artar, toplayıcı çalışmaya devam eder; `kill -9` → daemon yeniden başlatır.
- **Yan etki yok:** toplayıcı açıkken/kapalıyken 10 dk boşta ve sentetik DB yükünde MariaDB CPU farkı ölçülür (hedef:
  ≤ %0,05 çekirdek), game sağlık kaydında `late_pulses` farkı yok; canlı sunucuda toplayıcı kullanıcısıyla yazma denemesi
  reddedilir.
- **Sözleşme:** satırlar `m2metrics.py` tarafından adıyla okunur; 1 günlük gerçek boyut ölçülür; game/db hiçbir dosyasına
  dokunulmadığı kanıtlanır (`git diff --stat`).
- Kabul: yukarıdakilerin hepsi + `docs/monitoring.md` alan tablosu + worklog.

### Sonuçlar (2026-10-06, test VM, geçici MariaDB 11.8.9; canlı sunucuya kurulmadı)
| Kriter | Sonuç | Kanıt |
|---|---|---|
| Doğruluk | ✅ `row_lock_waits` 4 (2 bekleme + kilitlenmenin iki tarafı), `row_lock_time_ms` 2310, `table_locks_waited` 2, `deadlocks` 1, `com_insert` 40 | `integration-test.sh` T2 |
| MariaDB yeniden başlatma | ✅ tek `restart=1`, o satırda Δ yok | T3 |
| Kapalı MariaDB / geri çekilme | ✅ `up=0 err=connect`, 2 sn aralıkta 2 → 4 → 8 sn | T3 |
| Yetki kaybı | ✅ `err=auth` | T4 |
| Sadece izinli ifadeler | ✅ 32 `SHOW GLOBAL STATUS` + 5 bilgi `SELECT`, başka ifade yok | T5 (general log) |
| Disk dolu | ✅ satırlar atıldı, `write_errors>0`, süreç normal çıktı | T6 (64 KB tmpfs) |
| Maliyet / bellek | ✅ toplayıcı 650–1140 µs CPU/örnek, RSS 10.728 KB → 10.728 KB (300 örnek) | T7 (`procstat -r`, `ps`) |
| Kendi raporu doğru | ✅ `cpu_us` 647 µs vs `procstat` 650 µs | T7 |
| Root süreçleri ayrıcalıksız okunur | ✅ `mariadbd`, `channel1_core1` | T1 |
| Bağımlılık | ✅ sadece `libmariadb.so.3` + base (`libkvm`, `libdevstat`, libc++…); game/libsql/libthecore yok | `ldd` |
| Birim testleri | ✅ 10 grup, 0 uyarı | `build.sh` |

### Test VM kurulumu sonrası (2026-10-06, canlı MariaDB, oyuncu yok)
| Kriter | Sonuç | Kanıt |
|---|---|---|
| `m2stat` salt okunur | ✅ `SELECT account.account` 1142, `INSERT log.log` 1142, `CREATE DATABASE` 1044, `FLUSH TABLES WITH READ LOCK` 1227 (RELOAD), `SET GLOBAL` 1227 (SUPER) reddedildi; `processlist`'te sadece kendi oturumu (1 satır) | `su -m m2stat -c mariadb ...` |
| Çökme sonrası yeniden başlatma | ✅ `kill -9` → 30 sn sonra yeni pid, `first=1`; fırtına yok | `daemon -r -R 30` |
| Servis fd'leri | ✅ `daemon` ve toplayıcı fd 0 `/dev/null`; toplayıcı çıktısı `daemon` üzerinden syslog'a | `fstat` |
| Açık/kapalı (10'ar dk) | ⚠️ **Fark ölçülemiyor:** `mariadbd` CPU açık 403 µs/s, kapalı 763 µs/s (kapalı pencerenin sonunda saatlik yedek çalıştı); beklenen etki ~45 µs/s (0,45 ms/10 sn), boşta gürültünün çok altında. Üst sınır doğrudan ölçümden gelir (canlıda 0,45 ms/okuma). game: 300 satırda `late_pulses` açık 1 / kapalı 1 | `procstat -r`, `log/metrics_*.log` |
| Okuyucu | ✅ `m2metrics.py --dbstat`: `mariadbd` %0,041 çekirdek (`procstat` %0,040), toplayıcı %0,005; 11:59'daki geri yükleme testinin geçici `mariadbd`'si ayrı pid olarak göründü | canlı dosya |
| **Bekleyen** | ☐ 1 günlük gerçek boyut (kapsadığı güne göre ~18 MB beklenir); ☐ yük altında (roadmap 2.2) açık/kapalı | |
