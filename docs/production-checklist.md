# Production Kontrol Listesi

Son güncelleme: 2026-10-05

Sunucuya ilk gerçek oyuncu girmeden önce geçilmesi gereken kapılar. Her madde: ne, neden, nasıl doğrulanır.
Liste büyüdükçe buraya eklenir; kaynak: `docs/roadmap.md` Faz 1.

Durum: ☐ yapılmadı · ☑ yapıldı ve doğrulandı

## Güvenlik

### ☐ K-1 — P2P portları dışarıya kapalı

**Neden:** Çekirdekler arası P2P bağlantısında kimlik doğrulaması yok; gelen herkes "çekirdek" sayılıyor ve örneğin
bir çekirdeği kapattırabiliyor (`server-src/src/game/desc_manager.cpp:108-134`, `input_p2p.cpp:470-474`).
Orijinal Metin2 tasarımı bu portlara sadece sunucunun kendi süreçlerinin eriştiğini varsayar.

**Nasıl:** `deploy/freebsd/pf.conf` kuralını kiralık sunucuya uygula:
1. `ext_if` değerini sunucunun gerçek ağ kartı adıyla değiştir (`ifconfig -l`; çekirdeklerin P2P'yi bağladığı IP'nin
   hangi kartta olduğunu `sockstat -4l | grep 12011` ile kontrol et).
2. `<m2dev_servers>` tablosuna sunucunun kendi public IP'sini (ve çekirdekler birden çok makinedeyse hepsini) yaz.
   **Bir tabloyu olumsuzla (`! <tablo>`); satır içi liste (`! { a, b }`) kullanma** — pf'te herkesle eşleşir.
3. `pfctl -nf /etc/pf.conf` (sözdizimi), sonra `sysrc pf_enable=YES pf_rules=/etc/pf.conf`, `service pf start`.
4. Kiralık sunucuda SSH'ı kaybetmemek için ilk yüklemede zamanlayıcı kur:
   `daemon -f -p /var/run/pf-rollback.pid sh -c 'sleep 300; pfctl -d'`; yeni bir SSH bağlantısı çalışınca iptal et.
5. Hosting firmasının kendi firewall'u varsa ona **güvenme**; bu kuralı yine de kur (iki katman).

**Doğrulama (kapı — hepsi geçmeden sunucu açılmaz):**
- [ ] **Dışarıdan** (sunucunun dışındaki bir makineden) P2P portlarına TCP bağlantısı kurulamıyor:
      ```powershell
      12000,12011,12012,12013,12991 | % { "$_ : " + (Test-NetConnection <sunucu-ip> -Port $_ -WarningAction SilentlyContinue).TcpTestSucceeded }
      ```
      Hepsi `False` olmalı (engellenen bağlantı zaman aşımına uğrar, yanıt dönmez; ölçüm biraz sürer).
- [ ] Oyun portları (auth 11000, kanallar) ve SSH **dışarıdan erişilebilir** (`True`).
- [ ] Sunucu yeniden başlatıldıktan sonra çekirdekler birbirine bağlanıyor:
      `sockstat -4c | grep -E ":120[0-9]{2}|:12991"` — kanal başına çekirdekler arası tam mesh (4 çekirdek = 6 bağlantı = 12 satır);
      çekirdek `syserr.log`'larında hata yok.
- [ ] Haritalar arası geçiş çalışıyor: GM ile her çekirdeğin bir haritasına `/goto` (`#1`, `#21`, `#41`, bir CH99 haritası).
      **Bu P2P'yi sınamaz:** warp adresi db'den gelir ve client doğrudan hedef çekirdeğe bağlanır (`game/char.cpp:5371`). Sadece
      oyun portlarının, db'nin ve harita yüklemenin firewall'dan etkilenmediğini gösterir.
- [ ] **Çekirdekler arası P2P mesajı çalışıyor** (iki client, iki farklı çekirdekte iki karakter): GM `/notice <metin>` diğer
      çekirdekteki oyuncuda görünüyor (`GG::NOTICE`, `game/cmd_gm.cpp:1155`) ve GM `/transfer <isim>` diğer çekirdekteki
      oyuncuyu getiriyor (`GG::TRANSFER`, `game/cmd_gm.cpp:95-135`).
      Kanıt için `log.command_log` (GM komutları) ve diğer çekirdeğin `syslog.log`'u (`WarpSet <isim> … target map …`,
      `P2P: Login/Logout <isim>`) kullanılır; notice alımı log'a yazılmaz, oyunda görülerek doğrulanır.
- [ ] Kural açılışta kendiliğinden yükleniyor (`pfctl -s info` → `Enabled`, `pfctl -sr` kuralı gösteriyor) — VM'i yeniden başlatarak test et.

Referans: `docs/worklog/2026-10-05-p2p-firewall.md`.

### ☐ A-1 — Sunucu tarafı hile korumaları (karar gerekiyor)

Hız/saldırı hızı/kombo tespitleri şu an sadece log yazıyor, oyuncuyu atmıyor (`docs/roadmap.md` A-1). Açılıştan önce her biri için
**bilinçli karar** verilip kayda geçmeli: etkinleştir ya da bilerek kapalı bırak.
- [ ] Zaman hilesi (`input_main.cpp:1615-1622`) ve saldırı hızı sayacı (`char.cpp:7077-7090`) için tepki politikası kararı (log / uyarı / at / ban);
      kararın yan etkisi (arka plandaki pencere, yüksek ping gibi meşru durumlarda yanlış alarm) test edilmiş
- [ ] `CHECK_MULTIHACK` (şu an `0`) için karar
- [ ] `log.speed_hack` tablosu monitoring/panelde izleniyor

### ☐ K-3 — Yönetim kanalı

**Neden:** Repodaki şifre herkesçe biliniyor (upstream ile aynı; repoda artık yer tutucu `CHANGE_ME_BEFORE_PRODUCTION` var),
yönetim girişinde şifre log'a düşüyor (kod adımı yapılana kadar), `ADMINPAGE_IP` boşsa şifreyi bilen herkes yönetici olur.
Ayrıntı: `docs/roadmap.md` K-3, `docs/worklog/2026-10-05-admin-channel-config.md`.

- [ ] `game.txt`'de `ADMINPAGE_PASSWORD` rastgele ve uzun (ör. `openssl rand -hex 16`); yer tutucu ya da upstream değeri **değil**.
      Değer repo dışında saklanıyor, ekrana/log'a basılmadan üretildi.
- [ ] `ADMINPAGE_IP` dolu ve sadece gerekenler var (ör. `127.0.0.1` ya da yönetim servisinin IP'si). **Boş olmamalı.**
- [ ] `conf/` dosyaları `640`, klasör `750`; kurulumdan sonra `server/perms.py` (`0o777`) yeniden çalıştırılmadı (A-12).
- [ ] Log'larda şifre yok (değeri basmadan sayım):
      `P=$(grep '^ADMINPAGE_PASSWORD' <conf>/game.txt | sed 's/^[^:]*:[ ]*//' | tr -d '\r'); grep -rlF "$P" <channels>/ /var/log/m2dev.log | wc -l; unset P` → `0`
- [ ] Production'daki `game` binary'si port güvenliği ve log maskeleme düzeltmesini içeren kaynaktan derlenmiş
      (`docs/worklog/2026-10-05-admin-channel-code.md`). Açılışta `syserr.log`'da "publicly known default" uyarısı **yok**
      (uyarı varsa şifre varsayılan/yer tutucu değerde).

### ☐ K-2 — Hesap şifreleri

**Durum:** B yapıldı (`docs/worklog/2026-10-05-k2-password-review.md`): giriş özeti C++'ta hesaplıyor, SQL'e şifre gitmiyor,
`PASSWORD()`'a ve `old_passwords`'e bağımlılık yok; boş şifre sunucuda reddediliyor. Bu kapı, production `game` binary'sinin
bu kodu içerdiğini doğrular.

- [ ] Production `game` binary'si K-2 B'yi içeren kaynaktan derlenmiş (binary'de `SELECT PASSWORD(` metni yok:
      `strings <game> | grep -c "SELECT PASSWORD("` → `0`).
- [ ] Veritabanı sunucusu **MariaDB** (upstream şartı 11.8, `server-src/README.md:873`). B sonrası giriş için zorunlu değil, ama
      upstream'in desteklediği ortam bu.
- [ ] Genel sorgu log'u kapalı (`@@general_log = 0`); geçici açılırsa iş bitince kapatılıp log dosyası silinir.
- [ ] Gerçek bir hesapla giriş testi: doğru şifre `SUCCESS`, yanlış şifre `WRONGPWD`.
- [ ] Şifresi boş hesap yok: `SELECT COUNT(*) FROM account.account WHERE password=''` → `0` (B bunlara girişi zaten engelliyor).

## Yapılandırma temizliği (Faz 1.8)

- [ ] `common.gmhost` içinde `*.*.*.*` yok; GM erişimi sadece gerekli IP'lerle sınırlı
- [ ] Test hesapları (`admin`, `test`) ve test şifreleri kaldırıldı/değiştirildi
- [ ] **İzin/sahiplik doğrulaması (A-12) temiz** ve her deploy'un sonunda çalışıyor (`docs/engineering/a12-permissions.md` §7):
      kurulum ağacında ve derleme kaynağında grup/herkes yazılabilir öğe yok; root'un çalıştırdığı dosya ve dizinleri
      sadece yetkili deploy kimliği yazabiliyor; runtime yazma alanları sadece tanımlı yerler (`channels/*/`, `log/`,
      `share/mark`, `pids.json`). Kurulum arşivdeki modlara güvenmeden, modları normalleştiren yolla yapıldı.
- [ ] `perms.py` çalıştırılmadı (ya da `0777` ve `/var/db/mysql` bölümleri kaldırılmış sürümü); `/var/db/mysql` MariaDB
      varsayılanında (`mysql:mysql`, dizin `0700`, dosya `0660`), hiçbir dosya herkese okunur/yazılır değil
- [ ] Tanımsız giriş hesabı yok (`m2build` gibi); her hesabın amacı belgelenmiş, servis hesapları `nologin`
- [ ] MariaDB `sql_mode=NO_ENGINE_SUBSTITUTION` aktif ve ayar dosyası `mysql` kullanıcısı tarafından okunabilir (`docs/build-and-run.md` → Veritabanı)
- [ ] Sunucu açılışta kendiliğinden başlıyor (`service m2dev` ya da eşdeğeri) ve `start.py` konsola bağlı süreç bırakmıyor (`docs/worklog/2026-10-04-vm-autostart-sighup.md`)
- [ ] Client `serverinfo.py` üretim sunucusunu gösteriyor; "02. Test" kaydı kaldırıldı ya da gerçek bir test sunucusuna yönlendirildi
- [ ] Client exe `client-src`'den derlenmiş, hazır upstream exe değil
- [ ] MariaDB'de anonim kullanıcı yok: `SELECT user, host FROM mysql.user WHERE user = ''` → boş (test VM'de `''@localhost`,
      `''@<hostname>` var: sadece `USAGE`, ağ kapalı; varsayılan kurulumdan kalma)

## Veritabanı yedeği (Faz 1.6)

Ayrıntı: `docs/backup.md`. Test VM'de hepsi geçti (`docs/worklog/2026-10-05-db-backup.md`); production'da yeniden kanıtlanır.

- [ ] Saatlik yedek cron'da ve `status-backup-hot` `result=ok`; `lock_ms` ölçülüp kaydedildi (VM: 380–844 ms). Oyuncu varken
      bir yedek sırasında metriklerde (`docs/monitoring.md`) `late_pulses` artışı yok
- [ ] Yedekler sunucu **dışında** (ayrı yedek makinesi çekiyor; sunucu yedekleri silemiyor) ve şifreli; sunucuda sadece açık anahtar
- [ ] Özel anahtar en az iki ayrı yerde saklanıyor (kaybolursa bütün yedekler açılamaz)
- [ ] Günlük geri yükleme testi yedek makinesinde otomatik çalışıyor ve `status-daily` `restore_test=ok`
- [ ] Felaket geri yüklemesi (`docs/backup.md` → Geri yükleme) production'a benzer ortamda tatbik edildi: oyuna girilip karakter,
      envanter, depo ve yang karşılaştırıldı
- [ ] Binary/DB değişikliği prosedüründe `service m2dev stop` → `m2dev-backup consistent` adımı uygulanıyor
- [ ] **Yük testi (roadmap 2.2) sırasında** saatlik yedek de çalıştı: yedek anlarında metriklerde `late_pulses`/`iter_gap_max_us`
      artışı yok, syserr'de `[SLOW-GAME]` / `[SLOW-DB]` yok, `lock_ms` production veri büyüklüğünde ölçüldü
- [ ] Yedek root yerine ayrı bir MariaDB kullanıcısıyla (`MARIADB_EXTRA_FILE`); gereken yetkiler test edilip `docs/backup.md`'ye yazıldı

## Gözlemlenebilirlik (Faz 1.5)

- [ ] Sunucu makinesinde saat okuma maliyeti ölçüldü (`tools/metrics/clock-cost.cpp`) ve `sysctl kern.timecounter.hardware`
      kaydedildi. Test VM'de ACPI-fast ile okuma ~11,6 µs, metrik maliyeti süreç başına bir çekirdeğin ~%0,6'sı
      (`docs/monitoring.md` → Ölçüm maliyeti). Okuma ~1 µs'nin üstündeyse `METRICS_ENABLE` ve bölüm sayısı yeniden değerlendirilir.
- [ ] Her game sürecinde `log/metrics_<gün>.log` yazılıyor, `metrics_dropped` ve `metrics_write_errors` 0.
- [ ] `m2dev-dbstat` kurulu ve açılışta başlıyor (`docs/engineering/db-step1b-collector.md` → Kurulum): OS kullanıcısı
      `m2stat`, MariaDB `m2stat@localhost` sadece `USAGE` (unix_socket; `SHOW GRANTS` ile kontrol). `kind=db up=1 na=0`,
      bütün izlenen süreçler `kind=proc` satırında görünüyor (`security.bsd.see_other_uids=1`). Datadir ZFS'teyse `--disk`.
- [ ] **Saat senkronizasyonu doğrulandı** (ör. `ntpd`/`chronyd` çalışıyor ve senkron; komut çıktısıyla kaydedildi): farklı
      telemetri kaynakları ve yedek makinesi zaman damgasıyla birleştiriliyor. Test VM'de `ntpd` kapalı (`ntpd_enable=NO`);
      VirtualBox'ın saati kendisi senkronlayıp senkronlamadığı **doğrulanmadı**.
- [ ] **Teşhis açıkları kapandı** (`docs/roadmap.md`): T-1 `syserr.log` yeniden başlatmada korunuyor; T-2 / 1.9 çalışan
      binary'nin sürümü makinece okunur ve devreye alma kaydıyla eşleşiyor (aşağıdaki T-2 kapısı); T-3 disk boş alanı ölçülüyor (datadir, log ve
      yedek dizinleri); T-4 servis durumu süreç bazında doğru (tek çekirdek çökünce sağlıklı görünmüyor).
- [ ] **Derleme kimliği (T-2 / 1.9)** (`docs/build-and-run.md` → "Derleme kimliği ve kurulum"): production binary'leri git'li
      derleme makinesindeki temiz-git release script'iyle derlendi (bu yol fiilen test edildi) ve
      `m2dev-install-binaries.sh` varsayılan `production` politikasıyla kuruldu (sadece `src=git dirty=0`); her çalışan game/db
      sürecinin `version.txt`/`VERSION.txt`'i, telemetri satırlarındaki `build=` ve `deploy.log`'daki son `result=installed`
      satırı aynı commit'i gösteriyor; `share/bin/{game,db}` SHA-256'sı o satırla aynı. Kanıtın sınırı: işaret bir beyan,
      SHA-256 tam dosya; imzalı derleme değil.
