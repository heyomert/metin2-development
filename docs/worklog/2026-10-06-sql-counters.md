# AsyncSQL kuyruk, bekleme ve hata sayaçları (DB adım 1c)

- **Tarih:** 2026-10-06
- **Tür:** özellik / karar / ortam
- **Alan:** server-src / runtime / tools
- **Durum:** Aktif
- **PR / commit:** https://github.com/heyomert/metin2-development/pull/17

## Problem / hedef
1a, AsyncSQL'in bugünkü hatalarını geçici bir MariaDB'de kanıtladı: takılan kuyruk, tekrar edilmeyen 1205/1213/2013,
kapanışta kaybolan kopya kuyruğu. Canlıda bunların hiçbiri görünmüyordu. Hedef: davranışı **değiştirmeden** her SQL
bağlantısının kuyruk, bekleme ve hata durumunu ve db'deki SAVE sonuçlarını düşük maliyetle görünür yapmak. Bu, AsyncSQL
düzeltmesi ve InnoDB dönüşümünün önce/sonra karşılaştırmasının tabanı. Tasarım, kanıtlar ve sonuçlar:
`docs/engineering/db-step1c-sql-counters.md`; alanlar: `docs/monitoring.md` → "SQL (sql_*.log)".

## Kök neden / kanıt
Açık olmayan noktalar:
- **SAVE sayımının yeri.** Cache flush kayıtları `dwIdent=0` ile gönderiliyor (`db/Cache.cpp:61,143,171`); handle'lar 1'den
  başlıyor (`db/Peer.cpp:29-30`) → `GetPeer(0)` her zaman NULL → `AnalyzeQueryResult` SAVE `case`'lerinden önce
  dönüyor (`db/ClientManager.cpp:2511-2515`). Sayaç oraya konsaydı oyuncu/item kayıtlarının **hepsi** kaçırılırdı. Sayım
  fonksiyonun girişinde. Canlı doğrulama: item 27 sayıldı = MariaDB'nin gördüğü 27 `REPLACE`; oyuncu 1 = 1 `UPDATE`.
- **`player/main` hatası ≠ SAVE hatası.** db'deki bütün ReturnQuery'ler varsayılan slot `SQL_PLAYER`'dan geçiyor
  (`db/DBManager.h:43`), yükleme ve login sorguları dahil → SAVE'ler QID bazında ayrıca sayılıyor.
- **`uiSQLErrno` tekrar sonrası başarıda temizlenmiyor (yeni hata, S12 10/10).** Okuyan tek yer `QID_LOGIN_BY_KEY`
  (`db/ClientManagerLogin.cpp:137`). 1c düzeltmedi; adım 2'ye taşındı (`db-step1-measurement.md` §9). SAVE sayımı için bilgi
  amaçlı `uiFinalErrno` eklendi.
- **Kapanış flush'ının sonuçları hiç işlenmiyor** (`MainLoop` bitmiş): canlıda bağlantı düzeyinde `ok=6`, `save_*` 0.
  Bilinen sınır; telemetri için kapanış akışı değiştirilmedi.
- **game'de kapanış kaybı ölçülemez:** `DBManager` yıkıcısı `log_destroy()`'dan sonra (`game/main.cpp:423`).
- **Başlık sırası tuzağı (upstream):** `libsql/AsyncSQL.h`'nin `QUERY_MAX_LEN` makrosu `common/length.h`'deki aynı adlı enum
  değerini bozar; AsyncSQL önce dahil edilirse derleme kırılır. `server_metrics.h` bu yüzden raporlayıcıyı ileri bildirimle tutar.
- **Ortak havuz `metrics_dropped`'ın anlamını değiştirirdi:** alan havuzun `discard_counter()`'ı → her akış kendi havuzunda.

## Reddedilen yaklaşımlar
- **SAVE sayımı `AnalyzeQueryResult`'ın SAVE `case`'lerinde:** cache flush kayıtlarını kaçırır (yukarıda).
- **SQL satırlarını game sağlık satırına eklemek / aynı havuzu paylaşmak:** sağlık satırı biçimi ve `metrics_dropped`
  anlamı değişirdi.
- **Yazıcıyı `libthecore`'a koymak:** syslog ile aynı arıza alanı.
- **`CountQuery()` (kilit) ile kuyruk okumak:** kopya kuyruğunu görmüyor; monoton atomiklerden türetiliyor.
- **Parti alma anıyla yaş:** tıkanmada ana kuyrukta bekleyenleri göremez; mesaj başına ucuz saat damgası.
- **game kapanış sırasını değiştirmek:** sadece telemetri için davranış değişikliği olurdu.

## Çözüm
`libsql/AsyncSQL`: sadece ekleme (silinen tek satır kurucunun başlatıcı listesi) — monoton sayaçlar, `CollectStats()`,
mesaj başına `CLOCK_MONOTONIC_FAST` damgası, `uiFinalErrno`. `common/metrics_writer.h` (PR #12'deki yazıcı deseni; her
akış kendi kuyruğu/worker'ı) + `metrics_daily_sink.h` (game'den aynen taşındı) + `sql_metrics.h` (`kind=sum` her 10 sn,
`kind=conn` anormallikte/5 dk'da/başta/sonda, kümülatif `*_total`). game: `CServerMetrics` ortak yazıcıyı kullanıyor + ayrı
SQL akışı. db: `DBMetrics`, `METRICS_ENABLE` (`conf/db.txt`). Araçlar: 1a'ya `stats` satırları ve S12,
`enqueue-bench.cpp`, `sink-test.cpp` ortak bileşene ve iki izolasyon senaryosu, `m2metrics.py --sql`.

## Doğrulama
`docs/engineering/db-step1c-sql-counters.md` §11: davranış önce/sonra aynı (12 senaryo), sayaçlar 13 senaryoda gerçekle
uyuşuyor, üretici +50–55 ns/sorgu (medyan), izolasyon 10/10, test VM'de gerçek oyunla uçtan uca (SAVE sayıları MariaDB
sayaçlarıyla birebir). Bir saatlik oyuncusuz saha kontrolünde süreç CPU'sunda ölçülebilir artış yok (fark VM gürültüsü
içinde, sonuç çıkarılmadı); bu ölçümde hacim yaklaşık 24 MB/gün (6 süreç, oyuncusuz; yükte ölçülmedi), düşen/yazılamayan
satır 0.

## Bir dahaki sefere tuzaklar
- 1a aracının `stats` satırına zamanlamaya bağlı değer koyma (S9b `exec_n`).
- Bu ortamda Bash heredoc'u `\\n` kaçışlarını bozuyor; C++ string'i içeren düzenleme betiklerini dosyaya yaz.
- FreeBSD `sh`'ta `<( )` yok; `paste -sd' '` dosya argümanı ister (`-`).
- Zaman damgası: spdlog `+03:00`, `strftime` `+0300` (`docs/monitoring.md` → Telemetri sözleşmesi).
