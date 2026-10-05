# Veritabanı yedeği ve geri yükleme testi

Roadmap 1.6. Kaynak: `deploy/freebsd/backup/` (sunucu), `deploy/backup-host/` (yedek makinesi). Karar ve test kanıtları:
`docs/worklog/2026-10-05-db-backup.md`.

**Agent'lar için:** "Son yedek ne zaman, geri yüklenebiliyor mu?" sorusunun cevabı sunucuda
`/var/backups/m2dev/status-backup-hot`, `status-backup-consistent`, `status-restore-test` ve yedek makinesinde
`status-pull`, `status-daily` dosyalarında: tek satır `anahtar=değer` (`result=ok|fail`, `restore_test=ok|fail`).

## Ne yapar

| Parça | Nerede | Ne zaman | İş |
|---|---|---|---|
| `m2dev-backup hot` | game sunucusu | saatte bir, :17'de (`/usr/local/etc/cron.d/m2dev-backup`) | Oyun çalışırken mantıksal yedek |
| `m2dev-backup consistent` | game sunucusu | bakım/güncelleme öncesi, **servis durdurulmuşken** | Kayıpsız yedek: `db` kapanırken bütün önbelleği yazdı (`server-src/src/db/ClientManager.cpp:191-233`) |
| `m2dev-restore-test` | yedek makinesi (test aşamasında sunucuda, anahtar ssh üzerinden stdin'den) | günlük | Yedeği boş, geçici bir MariaDB'ye yükleyip doğrular. Canlı veritabanına dokunmaz |
| `m2dev-pull-backups.sh` | yedek makinesi | günlük görevin parçası | Yedekleri **çeker** (sunucu itemez, silemez), sha256 doğrular, saklama uygular |
| `m2dev-backup-host-daily.sh` | yedek makinesi | günde bir | Çekme + en yeni yedeğin geri yükleme testi → `status-daily` |

## Yöntem ve neden
- **Mantıksal döküm (`mariadb-dump`), fiziksel kopya değil.** Oyuncu tabloları (`player.player`, `player_index`, `safebox`…)
  Aria. `mariadb-backup` ile sıcak alınan kopya bu ortamda 2/2 denemede geri yüklenemedi (`player_index`: "must be
  zerofilled", zerofill sonrası "Can't find key for index"); canlı tablo sağlamdı. MariaDB: Aria için `--prepare`'de tam
  redo yok, "Won't Fix" ([MDEV-18573](https://jira.mariadb.org/browse/MDEV-18573)). Döküm geri yüklenirken tablolar ve
  indeksler yeniden kurulduğu için bu tür tutarsızlık oluşamaz.
- **Tek an:** ayrı bir oturum `FLUSH TABLES WITH READ LOCK` alır (yazmalar bekler, okumalar sürer). Kilit altında:
  tablo listesi, satır sayıları, `CHECKSUM TABLE ... EXTENDED`, yang toplamları (manifest) ve `account common player` +
  kullanıcı/yetki dökümü. Manifest ile döküm aynı ana ait olduğu için geri yükleme testi **sıcak yedekte de** birebir
  karşılaştırır. `log` (sadece ekleme alan tablolar) kilit bırakıldıktan sonra dökülür; büyüklüğü kilidi uzatmaz.
- **Kilit güvenliği:** global okuma kilidi istendiği anda yeni yazmaları durdurur, sonra **çalışmakta olan yazmaların**
  bitmesini bekler (`lock_wait_timeout` bu beklemeyi sınırlamıyor). Oyunun ana thread'inde birkaç senkron yazma var
  (ör. `server-src/src/game/guild.cpp:77` lonca kurma, `game/char_change_empire.cpp:169`, `game/db.cpp:405` auth'ta
  `last_play`); bu bekleme o çekirdeği dondururdu. İki koruma:
  1. 1 sn'den (`LONG_QUERY_SKIP_S`) uzun süren bir sorgu varsa yedek **kilit almadan** o çalışmayı atlar.
  2. Kilit 2 sn'de (`LOCK_WAIT_TIMEOUT`) alınamazsa script isteği `KILL QUERY` ile iptal eder; yedek sonraki saatte denenir.

  Ölçüm (VM, 8 sn süren hiçbir şeyi değiştirmeyen `UPDATE` sırasında yedek): korumasız bir yazma 7.494 ms, korumalı
  1.806 ms bekledi. Uzun bir **okuma** kilidi bekletmiyor (25 ms). Script herhangi bir anda ölürse (`kill -9` dahil)
  kilit oturumu kapanır ve MariaDB kilidi bırakır (test edildi).
- **Aria tablo kilidi (yedekten bağımsız):** Aria/MyISAM tabloyu bütün olarak kilitler. Bir tabloda uzun bir okuma
  sürerken o tabloya yazmak isteyen bekler. Ölçüm: `player.player`'da 8 sn'lik `SELECT` sırasında yedek **yokken** bir
  yazma 6.974 ms bekledi; başka tabloya yazma 1 ms. Canlı sunucuda elle ağır sorgu çalıştırma (roadmap: Aria → InnoDB).
- **Karakter seti:** `--default-character-set=binary`: latin1 tablolardaki ham baytlar birebir yazılıp okunur; geri
  yükleme testi `CHECKSUM TABLE` ile doğrular.
- **Kullanıcılar:** `--system=users --insert-ignore` → `CREATE USER IF NOT EXISTS`. Yeni sunucuda var olan sistem
  hesapları (root, mysql, mariadb.sys) değişmez, oyunun `mt2` kullanıcısı yetkileriyle gelir. (`--replace` root'u
  geri yükleme ortasında yeniden yaratıp yetkisiz bırakıyordu: ERROR 1698.)
- **Şifreleme:** `age`, sunucuda sadece açık anahtar. Özel anahtar operatörde/yedek makinesinde; sunucu ele geçirilse
  eski yedekler okunamaz. `age` içerik bütünlüğünü de korur (kurcalanmış dosya çözülmez, test edildi).

## Dosyalar
- Sunucu: `/var/backups/m2dev/m2dev-<UTC>-<hot|consistent>.tar.zst.age` + `.meta` (boyutlar, gizli bilgi yok) +
  `.sha256` (**en son yazılır**: `.sha256` yoksa dosya tamamlanmamıştır). Sunucuda en yeni `LOCAL_KEEP` (48 = iki günlük saatlik
  yedek; yedek makinesi bir gün çekemezse kayıp olmaz) tutulur.
- Arşiv içi: `m2dev-manifest`, `locked.sql`, `unlocked.sql`, `dump.err`.
- Durum satırları: `status-backup-<mode>` (`lock_ms`, `dump_kb`, `size_bytes`, `duration_s`), `status-restore-test`
  (`tables`, `compared`, `mismatch`, `check_warnings`); hepsi ayrıca `/var/log/m2dev-backup.log`.
- Yedek makinesi saklama (UTC, dosya adındaki zamana göre): son 48 saat hepsi; 14 güne kadar günde bir (en yenisi);
  8 haftaya kadar 7 günlük dilim başına bir (dilimler sabit, Perşembe–Çarşamba; ISO haftası değil); son 5 `consistent`
  her zaman. Saatlik yedekle ~71 dosya. 70 günlük sahte adla bağımsız hesapla doğrulandı (1.687 dosyada aynı 73).
- `/var/log/m2dev-backup.log`: `newsyslog` 1 MB'ta döndürür, 5 sıkıştırılmış kopya
  (`/usr/local/etc/newsyslog.conf.d/m2dev-backup.conf`). Geri yükleme testinin geçici klasörü her testten sonra, yarıda
  kalan yedeğin artıkları sonraki çalışmada silinir.

## Kurulum (FreeBSD sunucu)
```sh
pkg install age                                   # mariadb-backup/mariadb-dump, zstd: MariaDB paketi ve base
install -m 755 deploy/freebsd/backup/m2dev-backup.sh /usr/local/sbin/m2dev-backup
install -m 755 deploy/freebsd/backup/m2dev-restore-test.sh /usr/local/sbin/m2dev-restore-test
install -m 640 deploy/freebsd/backup/m2dev-backup.conf.sample /usr/local/etc/m2dev-backup.conf
mkdir -p /usr/local/etc/cron.d /usr/local/etc/newsyslog.conf.d
install -m 644 deploy/freebsd/backup/m2dev-backup.cron /usr/local/etc/cron.d/m2dev-backup
install -m 644 deploy/freebsd/backup/m2dev-backup.newsyslog.conf /usr/local/etc/newsyslog.conf.d/m2dev-backup.conf
# açık anahtar (age1...) -> /usr/local/etc/m2dev-backup.recipients  (özel anahtar sunucuya KONMAZ)
```
Anahtar üretimi (özel anahtar sunucu diskine yazılmadan): `ssh <sunucu> age-keygen > m2dev-backup.agekey` (yedek
makinesinde), açık anahtar dosyadaki `# public key:` satırı. **Özel anahtarın ikinci kopyası ayrı yerde saklanmalı;
kaybolursa bütün yedekler açılamaz.**

`MARIADB_EXTRA_FILE`: istemcilere ilk argüman olarak verilen `--defaults-extra-file` (soket ya da ayrı yedek kullanıcısı
ve şifresi, `0600`). `MYSQL_UNIX_PORT` ortam değişkeni `my.cnf`'yi geçersiz kılmıyor (VM'de denendi), bu yüzden bu yol.
Production'da root yerine ayrı bir yedek kullanıcısı önerilir; gereken yetkiler (en azından `RELOAD` (FTWRL), `PROCESS`,
`SELECT`, `SHOW VIEW`, `TRIGGER`, `EVENT`, `mysql` şemasında kullanıcıları okumak) production kurulumunda test edilip
yazılacak (**Bilinmiyor**: VM'de root ile test edildi).

Script'ler `PATH`'i kendileri kurar ve eksik komutu adıyla bildirir (cron'un `PATH`'i kuruluma göre değişir; kısıtlı
`PATH`'te ilk sürüm "MariaDB is not running" diye yanıltıyordu).

**Yedek makinesi:** `deploy/backup-host/` içindeki iki script, günde bir
`m2dev-backup-host-daily.sh <ssh host> <yerel klasör> <özel anahtar>`. Production'da ayrı bir FreeBSD makinesi
(`RESTORE_ON=local`: MariaDB + `deploy/freebsd/backup` kurulu; sunucuda yedek klasörünü sadece okuyabilen ayrı ssh kullanıcısı).
Test aşamasında geliştirici makinesi: Windows Görev Zamanlayıcı `m2dev-backup-daily`, her gün 12:00 (kaçırılırsa bilgisayar
açılınca); script kopyaları `C:\Users\mertw\.m2dev\bin\`, yedekler `C:\Users\mertw\.m2dev\backups\`, anahtar
`C:\Users\mertw\.m2dev\secrets\`. Çekme şimdilik VM'in root ssh'ıyla. Script repoda değişirse kopyaları yenile.

## Geri yükleme (felaket)
1. `service m2dev stop`. Mevcut veritabanı açılabiliyorsa önce onun da yedeğini al (`m2dev-backup consistent`).
2. Yedeği seç; doğrula: `m2dev-restore-test <dosya> <anahtar>` → `result=ok`.
3. Çöz: `age -d -i <anahtar> <dosya> | zstd -d | tar -xf - -C <boş klasör>`.
4. Canlı veritabanında `account common player log` şemalarını kaldır (DROP), sonra sırayla yükle:
   `mariadb --default-character-set=binary < locked.sql`, ardından `unlocked.sql`.
5. `mariadb-check --all-databases`, sonra `service m2dev start`, giriş + karakter + envanter kontrolü.

Adım 4 canlı veriyi değiştirir: production'da yapılmadan önce bu prosedür test VM'de tatbik edilmiş olmalı
(`docs/production-checklist.md`).

## Bilinen sınırlar
- **Sıcak yedek son dakikaları içermez:** `db` oyuncu verisini 7 dk, item'ları 5 dk önbellekte tutuyor
  (`server-src/src/db/Main.cpp:29-30`). `db` çökerse o dakikalar hiçbir yedekte yoktur. Kayıpsız nokta: `consistent`.
- **Kilit süresi veriyle büyür:** VM'de 380–542 ms, oyuncu varken 464–844 ms (manifest ~80 ms, döküm ~390 ms; veri
  ~1,7 MB). Bu sürede `db`'nin yazmaları kuyrukta bekler; oyunun log yazmaları zaten ayrı thread'den
  (`server-src/src/game/log.cpp:41`). Oyuncu varken 6 yedekte oyuncunun çekirdeğinde 13 pencerenin 12'sinde gecikme 0;
  ilk yedekte tek pulse (~17 ms) gecikme oyuncusuz çekirdekte de aynı anda görüldü (VM CPU'su), oyuncu hissetmedi. `lock_ms`
  izlenir; uzarsa `CHECKSUM_TABLES` daraltılır ya da tablolar InnoDB'ye geçirilip kilitsiz yönteme dönülür (ayrı karar).
- **Ölçeklenme sınırı (ölçüldü):** kilit `player.item` satır sayısıyla doğrusal büyüyor (geçici sunucuda, bugünkü veri +
  çoğaltılmış item): 108 → 0,44 sn; 110 bin → 0,71 sn; 1,77 milyon → 4,3 sn (manifest 1,4 + döküm 3,0); 3,54 milyon →
  8,0 sn. Bu sürede bütün yazmalar bekler (`db`'nin senkron yazmaları ve `game`'in ana thread yazmaları dahil). Küçük
  veride sorun yok; milyonlarca item'da kabul edilemez. Kalıcı çözüm Aria → InnoDB (`--single-transaction`, kilitsiz,
  boyuttan bağımsız) — karar bekliyor (roadmap teknik borç). `lock_ms` her yedekte kaydediliyor.
- **Belirli bir dakikaya dönüş yok** (binary log kapalı). Gerekirse ayrı iş: binlog + PITR.
- Geri yükleme hedefi aynı (ya da uyumlu) MariaDB sürümü olmalı; geri yükleme testi uyumsuzluğu yakalar.
