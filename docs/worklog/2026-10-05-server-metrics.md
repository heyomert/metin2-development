# Sunucu sağlık kaydı (metrics): izole, 10 sn'lik makine-okunur satır

- **Tarih:** 2026-10-05
- **Tür:** özellik / karar / ortam
- **Alan:** server-src / runtime
- **Durum:** Aktif
- **PR / commit:** —

## Problem / hedef
Roadmap 1.5, 1. adım. Ana döngü sağlık verisini hesaplıyor ama yazmıyordu: `main.cpp`'deki `s_dwProfiler` sayaçları
her saniye sıfırlanıyordu, okuyan yer yoktu. `heart_idle` sadece 30 sn üstü gecikmeyi logluyor (`heart.cpp:78-82`),
kısa takılmalar iz bırakmıyordu. Hedef: production öncesi sunucuyu gözlemlenebilir yapmak. Kural: **gözlem sistemi
gözleneni bozamaz.** Metrik kaybı kabul edilir, oyun döngüsünün beklemesi ya da çökmesi kabul edilmez. Alan sözleşmesi:
`docs/monitoring.md`.

## Kök neden / kanıt (tasarımı belirleyenler)
- syslog/syserr global spdlog havuzunu paylaşıyor: kuyruk 16.384, 1 thread, `block` (`libthecore/log.cpp`). Metrik
  sink'i orada takılırsa syslog da durur ve game thread beklerdi → **kendi havuzu** (kuyruk 64, 1 thread,
  `discard_new` → `enqueue_if_have_room`).
- `SPDLOG_LOGGER_CATCH`, `std::exception` olmayanı yeniden fırlatıyor (`spdlog/logger.h`) → worker'da terminate.
  Sink gövdesi `try{}catch(...){}`.
- `libthecore` STATIC, game ve db ikisi de bağlı → syslog sink'ine dokunulmadı; metrik tamamen `game/` içinde.
- `db_clientdesc->Update(t)`'deki `t` 5 dakikalık kanal durumu zamanlaması için kullanılıyor (`desc_client.cpp`)
  → `t` aynen kaldı, ölçüm ayrı monoton saatle.
- `m_set_pkChrState` oyuncu + mob/NPC (`char.cpp` `StartStateMachine`) → alan adı `fsm_chars`, "aktif mob" değil.
- **Saat maliyeti:** VM'de `steady_clock::now()` ~11,6 µs (`kern.timecounter.hardware: ACPI-fast`, TSC kalitesi 0;
  `tools/metrics/clock-cost.cpp`). Plandaki "onlarca ns" tahmini yanlıştı. Bölümler zaman damgasını paylaşıyor:
  tur başına 4 + 2×pulse okuma.

## Testte bulunan ve düzeltilen hatalar
1. **Gecikme yanlış pencerede görünüyordu.** Pencere tur sonunda kapanıyordu. `heart_idle` gecikmeyi bir sonraki turun
   başında döndürdüğü için gecikme bir sonraki satıra kayıyordu. Gözlenen: `max_late_pulses=719` (~12 sn) olan satırda
   `work_max_us=25895`, 12 sn'lik tur ise önceki satırda. Düzeltme: pencere, tur başında pulse'lar eklendikten sonra
   kapanıyor. Sonra açılış satırı `work_max_us=10794180` ile `max_late_pulses=647`'yi birlikte gösterdi (647/60 = 10,8 sn).
2. **Duraklama `heart_idle`'ın uykusunda olunca gecikme yine kayabiliyor.** `heart_idle` kaçan pulse'ları uykudan önce
   hesaplıyor (`heart.cpp:36-67`). `kill -STOP` 2 sn testinde duraklamanın penceresinde `pulses=493 late=0`, sonrakinde
   `late=119` çıktı. Çözüm: `iter_gap_max_us`, yani iki tur başı arasındaki gerçek süre (ek saat okuması yok).
   Tekrar testinde: `iter_gap_max_us=2018538 work_max_us=4811 late_pulses=120`, hepsi aynı satırda.
3. **Disk dolunca satırlar sessizce kayboluyordu.** Satır stdio tamponuna sığdığı için `fwrite` hiç hata vermiyor.
   ENOSPC ancak `fflush`'ta çıkıyor ve eski `flush_()` bunu saymıyordu. Kanıt (sink-test senaryo 8, 1 MB tmpfs):
   eski kod 256 kayıp satırda `write errors 0`, yeni kod `256` gösterdi (dosyada 45 satır + 256 = 301). Düzeltme:
   `sink_it_` her satırı yazıp `fflush` ediyor, hata sayılıyor. `flush_on(info)` kaldırıldı, çünkü her satır için
   kuyruğa ikinci bir mesaj ekliyordu.
4. Satırı yazma maliyeti bir sonraki `event_us`'a ekleniyordu → pencere kapanınca saat bir kez daha okunuyor, maliyet
   `other_us`'ta görünüyor.

## Bulunan, düzeltilmeyen (upstream davranışı)
- **`heart_idle` gecikmede fazla sayar:** geçen pulse'ın üstüne +1 dönüyor ve tabanı "şimdi"ye kaydırıyor
  (`heart.cpp:49-55, 70`). Her gecikmede oyun saati ~1 pulse öne geçiyor. Ölçülen: `late_pulses=79` olan 10,03 sn'lik
  pencerede 653 pulse (beklenen 601,6). Pulse'a bağlı zamanlayıcılar yük altında biraz hızlanır. `libthecore` db ile
  ortak olduğu için ayrı etki analizi gerekir; roadmap teknik borç.
- **VM genelinde kısa duraklamalar:** bütün süreçlerde aynı anda `late_pulses=1` ve `work_max_us` 2–4 ms. Bir kez de
  (20:09:29–20:10:05) ~35 sn boyunca bütün süreçlerde pencere başına 55–108 gecikme görüldü. STOP testi tekrarlanınca
  bu olmadı, host CPU'su %2–21'di. Kaynak: `Unverified` (VM/host zamanlaması). Ölçülenler: `select()` uyku aşımı en
  fazla ~9 ms (3.600 örnek), duvar/monoton saat farkı en fazla ±3 ms, VM'de `ntpd`/`VBoxService` yok, `kern.hz=100`,
  4 vCPU.

## Reddedilen yaklaşımlar
- Global spdlog havuzunu paylaşmak (`block` → game thread bekleyebilir) ve `libthecore` syslog sink'ini genelleştirmek
  (db'yi de etkiler; sink'te gün karışması ve error_code'suz `remove` var, ayrı küçük düzeltme önerisi).
- Mob sayısı için O(N) tarama: döngüye iş ekler.
- `dbc_us` ayrı bölümü: saat okuması pahalı olduğu için `io_us` içinde.

## Ortam olayı: VM askıya alındı, MariaDB açılmadı
Yeni binary dağıtıldıktan ~1,5 dk sonra VirtualBox VM'i askıya aldı (`VBox.log`: I/O cache `VERR_NO_MEMORY` →
`BLKCACHE_IOERR`). Windows commit 59,3/63,8 GB idi; misafir kodla ilgisi yok. Windows yeniden başlatılınca VM düzgün
kapanmadan kesildi:
- `/` düzgün ayrılmamış; diske yazılmakta olan `game` binary'si aynı boyutta ama farklı hash'le kaldı; geri dönüş
  binary'si yedeği kayboldu.
- MariaDB: `Aria engine: log data error ... log initialization failed → Aborting`.

Kurtarma (kullanıcı onayıyla):
1. Datadir'in soğuk kopyası alındı: `/root/mysql-cold-backup-20261005-postcrash`.
2. Salt okunur `aria_chk`: sadece `log.bootlog` bozuk.
3. `aria_log.*` silinmeden kenara taşındı (`/root/aria-log-corrupt-20261005/`), `aria_chk -r log/bootlog` çalıştırıldı
   (60 → 56 kayıt, açılış log'u).
4. MariaDB açılınca bütün Aria tabloları "Table is probably from another system and must be zerofilled" verdi: LSN
   değerleri yeni kayıttan ileride. MariaDB kapatılıp `aria_chk --zerofill` çalıştırıldı. 66 tablonun kayıt sayıları
   önce ve sonra birebir aynı, `aria_chk --check` temiz, `mariadb-check --all-databases` ile 98 tablonun 98'i OK.

## Doğrulama (test VM, 2026-10-05)
- Derleme 0 hata / 0 uyarı. `sink-test` 8/8 PASS (kuyruk doygunluğunda enqueue en fazla ~1,4 ms; 2.000 sinyalin 0'ı
  worker'da çalıştı).
- Süreç kapsamı: auth, ch1 core1–3, ch99'da `log/metrics_<gün>.log` var, db'de yok. `syslog`: `METRICS: enabled`.
- Boşta değerler: `pulses` 600–601, `window_ms` ≈ 10.000, `busy_pct` 1–2, `iter_gap_max_us` 17–25 ms.
- `kill -STOP` 2 sn: sadece durdurulan çekirdekte görüldü (yukarıda); diğerleri temiz.
- Kapanış: `CServerMetrics::Shutdown()` ve `thecore_destroy()` aynı ms'de; son kısmi pencere yazıldı.
- `kill -9`: son bayt `\n`, 1.000 satırda 0 bozuk; yeniden başlatınca yeni pid ile aynı dosyaya eklendi.
- Boyut: satır 401–417 B → süreç başına ~3,4 MB/gün.
- Oyuncu girişi: `log.loginlog2` ile birebir. Girişler 21:07:58, 21:12:45, 21:17:33 + 21:18:17; çıkışlar 21:12:14,
  21:16:46, 21:33:36/38 (ch1 core3). Metrik `users_local` 0→1 (21:08:10), →0 (21:12:21), →1 (21:12:51), →0 (21:16:51),
  →1 (21:17:41), →2 (21:18:21), →0 (21:33:42). Her geçiş ≤12 sn içinde: en fazla 1 pencere + ~5 sn oyuncu sayısı
  önbelleği. `pcs` ve `descs_total` (+1/oyuncu) birlikte değişti.
- Oyun sırasında (2 oyuncu, 26 dk, 162 pencere): `busy_pct` ortalama 1,33, en fazla 2,87; tek gecikme 21:21:41'de
  43 ms'lik bir tur (2 pulse).
- A/B (boşta, 5 süreç, 300 sn, `procstat -r`): açık süreç başına ortalama %1,56, kapalı %0,94 (bir çekirdeğin
  %'si) → fark ~0,6 puan; tahmin %0,4'tü. Sebep: VM'de her saat okuması sistem çağrısı + emüle edilen ACPI portu
  (`kern.timecounter.choice: TSC-low(0) ACPI-fast(900)`); oyunun boştaki CPU'sunun da çoğu sistem zamanı. Maliyet
  oyuncu sayısıyla artmaz (okuma sayısı tur/pulse başına, döngü 60/sn ile sınırlı). Production donanımında ölçüm
  `docs/production-checklist.md`'ye kapı olarak eklendi. Test sonunda `game.txt` yedekle aynı (`cmp`), metrikler açık.

## Bir dahaki sefere tuzaklar
- Windows'u yeniden başlatmadan önce VM'i düzgün kapat (`ssh bsd shutdown -p now`). Host belleği dolarsa VirtualBox
  VM'i askıya alır ve bekleyen disk yazmaları kaybolabilir.
- Aria kaydı silinip yeniden oluşturulursa tablolar "from another system" verir: MariaDB kapalıyken
  `aria_chk --zerofill` gerekir. Bu veri bozulması değildir; kayıt sayılarını önce/sonra karşılaştır.
- Oyun süreçleri root olarak çalıştığı için `chmod` ile `log/` yazılamaz yapılamaz; yazma hatası testi `sink-test`'te
  (tmpfs) yapılır.
