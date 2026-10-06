# MariaDB + OS toplayıcısı (DB adım 1b): bağımsız, salt okunur `m2dev-dbstat`

- **Tarih:** 2026-10-06
- **Tür:** özellik / karar / ortam
- **Alan:** runtime / db / tools
- **Durum:** Aktif
- **PR / commit:** https://github.com/heyomert/metin2-development/pull/15

## Problem / hedef
Bugünkü karışık Aria/MyISAM/InnoDB yapısının MariaDB ve işletim sistemi tarafını ölçmek; AsyncSQL düzeltmesi ve InnoDB
dönüşümü öncesi/sonrası A/B karşılaştırması için baseline. Şart: gözlem gözleneni değiştirmez; game/db/MariaDB
toplayıcıya bağımlı olmaz; en az yetki. Tasarım ve ölçümler: `docs/engineering/db-step1b-collector.md`; alanlar:
`docs/monitoring.md` → "MariaDB / OS (dbstat)".

## Kök neden / kanıt
Geliştirme sırasında bulunan, açık olmayan noktalar:
- **Süreç CPU'su pencere başına ms'ye kesilince küçük süreçler kayboluyordu.** `ki_runtime` µs; her 1 sn'lik Δ'yı
  `/1000` ile kesmek toplayıcının kendi CPU'sunu ~18 kat düşük gösterdi (236 örneğin 221'i `cpu_ms=0`; `procstat`:
  1140 µs/örnek, satır toplamı ~63 µs/örnek). Alan `cpu_us` oldu; entegrasyon testi artık kendi bildirdiğini
  `procstat` ile karşılaştırıyor (±%20).
- FreeBSD 15.1 `devstat`'ta `DSM_KB_PER_SECOND_*` yok, sadece `DSM_MB_PER_SECOND_*` → alanlar `mb_r_s`/`mb_w_s`.
- `INSERT ... SELECT` `Com_insert`'e değil `Com_insert_select`'e sayılıyor.
- MariaDB yeniden başlayınca general log'daki thread id'ler 1'den başlıyor; ifadeleri kullanıcıya bağlarken id'nin o
  anki sahibi sırayla izlenmeli.
- `security.bsd.see_other_uids=1` (varsayılan) ile ayrıcalıksız kullanıcı root süreçlerinin `ki_runtime`/`ki_rssize`'ını
  ve `argv[0]`'ını okuyabiliyor; game çekirdekleri `ki_comm`'da hepsi `game`, ayrım sadece `argv[0]` ile.
- `FLUSH STATUS` denenen sayaçları sıfırlamadı; olmayan değişken adı hata değil, eksik satır döner (→ `NA`).

## Reddedilen yaklaşımlar
- **sh + her örnekte yeni `mariadb` istemcisi + `ps`/`sysctl`/`iostat`:** ölçüldü, MariaDB tarafı 13 kat (5,8 ms vs 0,45 ms)
  ve bağlantı churn'ü; zaman aşımı/yeniden bağlanma kırılgan.
- **game/db içine ya da libthecore/libsql'e bağlamak:** gözlemciyi gözlenenle aynı arıza alanına koyar.
- **`PROCESS` yetkisi / `performance_schema`:** işlem listesi başka oturumların SQL metnini gösterir; `performance_schema`
  kapalı ve açmak MariaDB'yi değiştirir. Tablo kilidi süresi 1c'de istemci tarafında ölçülecek.
- **`Table_locks_waited`'dan süre türetmek:** sayaç sadece sayı; süre çıkarılamaz.

## Çözüm
`deploy/freebsd/metrics/m2dev-dbstat/`: C++20, sadece sistem `libmariadb` + FreeBSD base (`libkvm`, `libdevstat`);
kalıcı unix socket bağlantısı, `USAGE` yetkili kullanıcı (unix_socket), zaman aşımları (3/5/5 sn), otomatik yeniden
bağlanma kapalı, aralık→×2→60 sn geri çekilme; günlük dosya, satır başına `fflush`, yazma hatası sayılır;
`daemon -r -R 30` ile rc servis (henüz kurulmadı). `tools/metrics/m2metrics.py --dbstat` okuyucu.

## Doğrulama
VM'de: derleme 0 uyarı; 10 birim test grubu PASS; entegrasyon testi (geçici MariaDB, `nobody`/`USAGE`) T1–T7 PASS:
bilinen olaylar doğru alanda (2 InnoDB bekleme + kilitlenme, 2 Aria bekleme, 1 deadlock, 40 insert), MariaDB yeniden
başlatmada tek `restart=1` ve sıçrama yok, geri çekilme 2→4→8 sn, yetki kaybında `err=auth`, gönderilen ifadeler
sadece `SHOW GLOBAL STATUS` + bilgi `SELECT`, dolu diskte satır atılıp sayıldı, 300 örnekte RSS büyümesi 0.
Test VM'e kuruldu (`m2stat`, `USAGE`): yazma/okuma/kilit/`SET GLOBAL` reddedildi, `kill -9` → 30 sn sonra yeniden
başladı. Boşta 10'ar dk açık/kapalı karşılaştırması etkiyi ayırt edemedi (beklenen ~45 µs/s, gürültü yüzlerce µs/s;
kapalı pencerede saatlik yedek de çalıştı); game `late_pulses` iki pencerede de 1. Ayrıntı: tasarım dokümanı → Sonuçlar.
Okuyucuda CPU yüzdesi ilk–son satır arası duvar saatine bölünüyordu, toplayıcının kapalı olduğu boşluk paydaya
giriyordu (%0,02 vs gerçek %0,04) → her Δ kendi penceresine bölünüyor.
Maliyet: toplayıcı 0,65–1,14 ms CPU/örnek (iki koşu; kendi `cpu_us` satırı 647 µs, `procstat` 650 µs), RSS 10,7 MB; canlı MariaDB'de okuma 0,45 ms; çıktı ~18 MB/gün.

## Bir dahaki sefere tuzaklar
- **rc betiğinde `${name}_user` ayrılmış ad:** `/etc/rc.subr:955` bu değişken tanımlıysa komutu (`daemon`'un kendisini)
  `su -m` ile o kullanıcı olarak çalıştırır (`rc.subr:1535`) → `daemon: ppidfile /var/run/m2dev_dbstat.pid: Permission
  denied`. Değişken `m2dev_dbstat_runas` oldu: `daemon` root başlar, pidfile'ı root sahipliğinde açar, `-u` ile sadece
  toplayıcı `m2stat` olur (supervisor root kalır, kontrol edildi `ps`). Pidfile'ı kullanıcıya yazılabilir bir yere
  koymak reddedildi: o kullanıcı pidfile'ı değiştirip `service stop`'a başka süreci öldürtebilir.
- **`daemon(8)` `-f` olmadan stdin/stdout/stderr'i kapatmaz:** elle başlatılınca ssh oturumu asılı kaldı (`fstat`: fd 0–2
  ssh pipe'ları). `-f -S` ile fd'ler `/dev/null`, toplayıcı çıktısı syslog'a.
- ZFS datadir'de disk otomatik bulunamaz → `--disk`.
- `threads_running/connected` toplayıcının kendi bağlantısını içerir.
- `slow_queries` bugün 10 sn eşiğiyle neredeyse kör; 0 "yavaş sorgu yok" demek değil.
