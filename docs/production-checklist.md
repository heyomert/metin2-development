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
- [ ] **Kod adımı yapıldı:** yönetim girişi şifresi log'a yazılmıyor (`input.cpp:238`, `config.cpp:198`) ve A-11 değerlendirildi.

### ☐ K-2 — Şifre saklama yöntemi
Bkz. `docs/roadmap.md` (Faz 1.2). Ayrıntılı doğrulama maddeleri ilgili iş yapılırken buraya eklenecek.

## Yapılandırma temizliği (Faz 1.8)

- [ ] `common.gmhost` içinde `*.*.*.*` yok; GM erişimi sadece gerekli IP'lerle sınırlı
- [ ] Test hesapları (`admin`, `test`) ve test şifreleri kaldırıldı/değiştirildi
- [ ] MariaDB `sql_mode=NO_ENGINE_SUBSTITUTION` aktif ve ayar dosyası `mysql` kullanıcısı tarafından okunabilir (`docs/build-and-run.md` → Veritabanı)
- [ ] Sunucu açılışta kendiliğinden başlıyor (`service m2dev` ya da eşdeğeri) ve `start.py` konsola bağlı süreç bırakmıyor (`docs/worklog/2026-10-04-vm-autostart-sighup.md`)
- [ ] Client `serverinfo.py` üretim sunucusunu gösteriyor; "02. Test" kaydı kaldırıldı ya da gerçek bir test sunucusuna yönlendirildi
- [ ] Client exe `client-src`'den derlenmiş, hazır upstream exe değil
