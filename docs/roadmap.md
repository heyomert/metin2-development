# Roadmap

Son güncelleme: 2026-10-05 (açılış tanımı eklendi)

Bu dosya sadece bir istek listesi değil: fazların sırası, birbirine bağımlılıkları, riskleri ve "bitti" kriterleri.
Açık sorunlar ve teknik borç da burada tutulur. Bir madde tamamlandığında PR/worklog linkiyle işaretlenir.

**Temel ilke:** Yeni özellik eklemeden önce stabilite, performans, güvenlik ve veri bütünlüğü korunur.
Ölçülemeyen şey hakkında karar verilmez.

---

## Açılış tanımı

Roadmap'in önceliklerini bu bölüm belirler. Kullanıcının 2026-10-05 tarihli kararları; değişirse burada güncellenir.

| Konu | Karar |
|---|---|
| Hedef oyuncu | Açılışta **500 tekil oyuncu**. Kişi başı en fazla 3 client → **~1500 bağlantı**; reklam/pazarlamaya göre **~2000 bağlantıya** çıkabilir |
| Çoklu client kuralı | 1. ve 2. client drop alır; 3. client sadece exp alır; 4. ve sonrası açılamaz (bkz. F-1) |
| Hedef kitle | Türkiye |
| Sunucu | Kiralık, profesyonel sunucu (kendi donanım yok). Lag kabul edilmez: savaşlar ve kalabalık mob bölgeleri sorunsuz olmalı |
| Sistemler | Files tamamen custom olacak. Sistemler kullanıcının vereceği listeye göre kademe kademe eklenecek/güncellenecek (bkz. "Özellik listesi") |
| Zaman | Zaman hedefi yok. Öncelik kalıcı ve doğru ilerlemek |
| Test yaklaşımı | Kombinasyonlar test edilir, ama gereksiz yük oluşturulmaz → risk seviyesine göre test (`docs/engineering/change-impact.md`) |

### Bu kararlardan çıkan teknik gereksinimler

| # | Gereksinim | Neden |
|---|---|---|
| G-1 | **Kapasite hedefi: 2000 eşzamanlı bağlantı + pay.** Yük testleri en az ~2500–3000 bağlantıyla yapılmalı | Pazarlama tavanı 2000; testin hedefin üstünde olması gerekir |
| G-2 | **Sıcak nokta testi: tek haritada kalabalık.** Normal haritalar kanallara bölünür, ama savaş/etkinlik haritaları bütün kanallar için **tek çekirdekte** (`channel99_core1`, bkz. A-10). Yük testi "yüzlerce oyuncu aynı savaş haritasında" senaryosunu içermeli | Çekirdek tek thread'li; o haritadaki herkes tek thread'i paylaşır |
| G-3 | **Donanım seçiminde tek çekirdek (single-thread) performansı** belirleyici; çekirdek sayısı ikinci planda. Kesin donanım, Faz 2 yük testi ölçümleriyle seçilir — tahminle değil | `docs/architecture.md` → "Çalışma modeli" |
| G-4 | Kiralanacak sunucu: **FreeBSD** desteği (server şu an FreeBSD'de derlenip çalışıyor; Linux'ta çalışması doğrulanmadı), **DDoS koruması**, Türkiye'ye düşük gecikme | A-3; hedef kitle Türkiye |
| G-5 | **KVKK** kapsamı kesin (Türkiye): IP, e-posta vb. kişisel verilerin saklanma süresi, güvenliği, silinmesi | A-2 |
| G-6 | **Türkçe içerik:** sunucu quest/sistem metinleri şu an sadece İngilizce | A-6 |

---

## Fazlar

Her faz bir öncekine dayanır. Düşük riskli özellik işleri Faz 1–2 ile paralel yürüyebilir; **yüksek riskli değişiklikler**
(ekonomi, item akışı, paket yapısı, DB şeması) en az Faz 1 monitoring'i ve Faz 2 regresyon listesi hazır olmadan yapılmaz.

### Faz 0 — Temel ✅
Repo + upstream doğrulama, client kaynaktan derleme, VM sunucusu (`service m2dev`), MariaDB ayarı, `loginlog2` şema düzeltmeleri,
proje hafızası. → PR #1–#4, `docs/worklog/`.

### Faz 1 — Güvenlik temeli ve gözlemlenebilirlik
**Bağımlılık:** Faz 0. **Neden önce:** Ölçmeden yük, leak, stabilite hakkında karar verilemez; kritik açıklar production'a taşınmamalı.

| # | İş | Risk / not | Bitti kriteri |
|---|---|---|---|
| 1.1 | **P2P portlarını kapat** (K-1) — test VM ✅ 2026-10-05; **production ☐** | Kritik. Karar: `pf` firewall (`deploy/freebsd/pf.conf`), kod değişikliği yok. İç IP'ye bağlama reddedildi (db public IP'yi dağıtıyor, warp'ı bozar) — `docs/worklog/2026-10-05-p2p-firewall.md` | Dışarıdan 12xxx'e bağlanılamıyor, çekirdekler arası bağlantılar ve warp çalışıyor, kural açılışta yükleniyor. Production kapısı: `docs/production-checklist.md` K-1 |
| 1.2 | **Hesap şifreleri** (K-2) — 2026-10-05: **A ✅**, **B ✅ test VM** (C++ özet + boş şifre reddi), production ☐, C → Faz 3 | A: MariaDB + `old_passwords=0` + genel sorgu log'u kapalı (`docs/production-checklist.md` K-2). B: aynı `*SHA1(SHA1)` biçimini C++'ta üret → şifre SQL'e gitmez, `PASSWORD()` bağımlılığı biter, veri değişmez (plan: `docs/worklog/2026-10-05-k2-password-review.md`). C: modern hash (argon2id) — web sitesi/hesap sistemiyle birlikte, yük testinden sonra | B: C++ özeti `PASSWORD()` ile birebir aynı (Türkçe karakterler dahil), giriş testi geçti |
| 1.3 | Yönetim kanalı (K-3) — **config ✅ ve kod ✅ test VM 2026-10-05; production ☐** | Config: rastgele şifre (repo dışında), `ADMINPAGE_IP` dolu, conf `640/750`, repoda yer tutucu (`docs/worklog/2026-10-05-admin-channel-config.md`). Kod: upstream'in kaldırdığı `ENABLE_PORT_SECURITY` kontrolü geri getirildi (liste dışı IP → içerik log'lanmadan reddedilir), şifre log'larda maskelendi (`docs/worklog/2026-10-05-admin-channel-code.md`) | Herkesçe bilinen şifre yok, log'larda şifre yok, liste dışı IP komut kanalını kullanamıyor. Production kapısı: `docs/production-checklist.md` K-3 |
| 1.4 | Log saklama ve izinler | **Rotasyon kodda var:** `syslog.log` günlük `log/syslog_YYYY-MM-DD.log`'a arşivleniyor, 7 günden eskisi siliniyor (`server-src/src/libthecore/syslog_rotate_sink.h:46-70`). Hacim: çekirdek başına ~27 MB/gün. Açık: arşivler `644` (herkes okuyabiliyor), 7 gün yeterli mi (KVKK, A-2). **T-1 (teşhis açığı):** `syserr.log` her açılışta sıfırlanarak açılıyor (`server-src/src/libthecore/log.cpp:43`, `basic_file_sink_mt(..., true)`) → çöküp yeniden başlayan sürecin hata kaydı kaybolur. **AsyncSQL düzeltmesi devreye alınmadan önce** (önce/sonra karşılaştırmasında hata kanıtı korunmalı); libthecore'a dokunduğu için ayrı etki analizi | Saklama süresi ve izinler için bilinçli karar; disk kullanımı izleniyor; yeniden başlatmada önceki çalışmanın hataları duruyor |
| 1.5 | Monitoring — **game sağlık satırı ✅ test VM 2026-10-05** (döngü süresi, gecikme, duraklama, oyuncu/karakter sayısı, gönderilen bayt; `docs/monitoring.md`, PR #12). **MariaDB/OS toplayıcısı `m2dev-dbstat` ✅ test VM 2026-10-06** (süreç CPU/RSS, disk G/Ç, MariaDB kilit/kilitlenme/senkron; PR #15). **SQL kuyruk/bekleme/hata sayaçları (DB adım 1c): kabul testleri geçti, PR'da (merge bekliyor); test VM'de çalışıyor** (`log/sql_*.log`, `docs/engineering/db-step1c-sql-counters.md`) | Kalan: **T-3 (teşhis açığı): disk boş alanı hiçbir yerde ölçülmüyor** (dbstat G/Ç'yi ölçüyor, doluluğu değil; yedek, log ve MariaDB aynı diskte) → dbstat'a küçük ekleme, AsyncSQL işini bloklamaz. Grafik (Prometheus/Grafana + node_exporter, VM paket kurulumu → ayrı onay), dış nabız kontrolü, uyarılar. "Normal" aralıklar yük testinden sonra (`BASELINE_PENDING`) | Zaman serisi grafikleri; bellek artışı ve disk doluluğu görülebiliyor |
| 1.6 | Otomatik DB yedeği + **restore testi** — **test VM ✅ 2026-10-05**: saatlik `mariadb-dump` (tek global kilit, ~0,5 sn), `age` ile şifreli, yedek makinesi çekiyor, günlük otomatik geri yükleme testi, tatbikat oyunda doğrulandı → `docs/backup.md`; production ☐ | Yedek sunucu dışında ve şifreli. Fiziksel sıcak yedek (`mariadb-backup`) Aria tablolarında geri yüklenemedi (MDEV-18573) → mantıksal döküm. Sıcak yedek `db` önbelleğindeki son ~7 dk'yı içermez; dakika hassasiyeti (binlog + PITR) ayrı iş | Restore düzenli test ediliyor. Production kapısı: `docs/production-checklist.md` Faz 1.6 |
| 1.7 | Core dump + çökme uyarısı | Sessiz yeniden başlatma yok. Somut örnek: test VM'de `channels/db/db.core` (2026-10-05 19:52) hiçbir yerde kayıtlı değil; silinmeden ayrı incelenecek. **T-4 (teşhis açığı):** `service m2dev status` `pids.json`'daki süreçlerden **biri** canlıysa "çalışıyor" diyor (`/usr/local/etc/rc.d/m2dev` `m2dev_running`) → tek çekirdek çökse de sağlıklı görünür; süreç bazında bilgi bugün sadece dbstat `kind=proc` satırlarında | Çökmede dump + uyarı; sebebi bulunabiliyor; durum süreç bazında doğru |
| 1.8 | Production config temizliği | `gmhost *.*.*.*`, test hesapları, varsayılan değerler | Kontrol listesi geçti |
| 1.9 | **Sürüm kimliği (T-2, teşhis açığı)** — **mekanizma tamam, test VM'de doğrulandı (derleme + kurulum korumalı dizinde); çalışan binary'lerde ve production temiz-git yolunda henüz doğrulanmadı** | Kök neden (giderildi): sürüm configure anında `git describe` ile hesaplanıyordu, VM'de git yok → `unknown`; game'de makro adı uyuşmazlığı (`GIT_DESCRIBE` ↔ `GIT_DESCRIBE_VERSION`). Şimdi: kimlik her derlemede (`server-src/cmake/BuildIdentity.cmake`; `git` / `archive` / `injected` / `none`), binary'ye gömülü işaret, `version.txt`/syslog, telemetride `build=`, game+db birlikte doğrulayıp geri alabilen kurulum + `deploy.log` (SHA-256) (`docs/build-and-run.md` → "Derleme kimliği ve kurulum"). **Kalan:** (1) çalışan süreçlerde `version.txt` ↔ telemetri ↔ `deploy.log` eşleşmesi (deploy + yeniden başlatma, ayrı onay); (2) git'li FreeBSD derleme makinesinde tek giriş noktalı temiz-git release script'i ve production kurulumu (ayrı onay). Production politikası sadece `src=git dirty=0` kabul ettiği için o yol kurulana kadar production kurulumu **yapılamaz** (fail-closed). AsyncSQL düzeltmesi test VM'de bu mekanizmayla (`archive`) ölçülebilir | Her süreç açılışta sürümünü makinece okunur yazıyor; derleme commit'i build sırasında veriliyor; devreye alma kaydında binary özeti var; production temiz-git yolu fiilen test edildi |

### Faz 2 — Test altyapısı
**Bağımlılık:** Faz 1 (monitoring olmadan yük testi sonucu okunamaz).

| # | İş | Not |
|---|---|---|
| 2.1 | Regresyon kontrol listesi | Giriş, karakter seçimi, warp/çekirdek değişimi, ticaret, depo, item kullanma/düşürme, quest, ölüm/diriliş, çıkış türleri |
| 2.2 | Yük testi aracı (bot / headless client) | Zor: paket protokolü modernize edilmiş, şifreli handshake var. "Yüksek oyuncu sayısı" sorusunun tek gerçek cevabı |
| 2.3 | Ekonomi telemetrisi | Yang ve item'ın sisteme giriş/çıkışı; dupe şüphesi tespiti |

### Faz 3 — Yönetim servisi + Admin Panel (operasyon kontrol merkezi)
**Bağımlılık:** Faz 1 (telemetri ve durum dosyaları panelin ilk ekranları; T-1…T-4 kapanmış), Faz 1.3 (yönetim kanalı).
Mimari ve kalıcı ilkeler: `docs/architecture.md` → "Admin Panel mimari kararı", "Kontrol katmanı ilkeleri" (runtime
bağımlılığı yok; önce okuma sonra aksiyon; sadece tanımlı ve yetkili işlemler; yıkıcı işlemler buton değil; süreli
duraklatma; en az yetkiyle okuma; panel ve agent aynı veri kaynağı).
**Ürün gereksinimi:** operasyonel gözlem sadece insanlar için değil, **agent destekli olay teşhisi** için de. Olay
sırasında yetkili bir agent kodu taramadan güncel durumu salt okuyabilmeli (`docs/monitoring.md` → "Olay teşhisi").

| # | İş | Not |
|---|---|---|
| 3.1 | Yönetim servisi (backend) — **önce salt okuma** | Panelin ve teşhis yapan agent'ın konuştuğu tek yer; yetki, işlem kaydı, hız sınırı. Bugünkü kaynakları **olduğu gibi** okur (biçim değişikliği gerekmez): game `metrics_*.log`, `sql_*.log` (1c merge edilince), dbstat, yedek/restore-test `status-*` dosyaları, yedek makinesinin `status-pull`/`status-daily`'si, `pids.json`, servis durumları, sürüm kimliği (1.9). Gerekenler: salt-okuma grubu (bugün yedek durumları `0600 root`), makine başına toplayıcı (`host` alanı game/SQL'de süreç adı, dbstat'ta makine adı), JSON arayüz |
| 3.2 | Panel MVP — **sadece okuma** | Çekirdek sağlığı/yükü, oyuncu/bağlantı sayısı, db süreci, MariaDB CPU/RAM/disk/kilit, AsyncSQL kuyruk/takılma/hata/SAVE sayaçları, son yedek (hot/consistent, süre, kilit süresi, boyut, yaş), son restore testi, servis durumları, disk doluluğu, yeniden başlatma/çökme geçmişi, çalışan sürüm, önemli hatalar, production kontrol listesi durumu; hesap/karakter görüntüleme |
| 3.3 | Kontrollü işlemler — sadece tanımlı işlem kataloğu | Oyun: duyuru, kick, ban/mute, etkinlik, bakım modu (komut kanalı kararı: `architecture.md` ilke 8). Operasyon: manuel yedek, restore **testi** (geçici örnekte; canlıya dokunmaz), yedek planını görme, yedeği **süreli** duraklatma (neden, kim, otomatik açılma), kontrollü servis yeniden başlatma. Production restore panelden sıradan işlem olmaz (ayrı tören) |
| 3.4 | Çevrimdışı veri işlemleri | Sadece hesap çevrimiçi ve db önbelleğinde değilken, işlem kaydıyla |
| 3.5 | Güvenlik sinyalleri aynı yüzeyde | Yönetim kanalı redleri (K-3), `log.speed_hack` (A-1), başarısız girişler, pf blokları |

Hazır araçlar (ör. monitoring için Grafana/Prometheus) değerlendirilmeli; panel oyuna özgü işlemlere ve operasyon
durumuna odaklanmalı. **Faz 3 entegrasyonu için mevcut telemetri biçimlerini yeniden yazmayı gerektiren zorunlu bir iş
yok:** game sağlık satırı, dbstat ve yedek/restore-test durum dosyaları zaten makinece okunur; Faz 3'te biçimleri
değiştirilmez, okuma yetkisi ve toplama eklenir. Bu, production öncesi işlerin bittiği anlamına gelmez: T-1…T-4 (1.4,
1.5, 1.7, 1.9) ayrıca kapanmalı.

### Faz 4 — Özellik geliştirme
Her özellik: etki analizi (`docs/engineering/change-impact.md`) + test + monitoring/log + panel ihtiyacı tasarımda düşünülmüş.

## Özellik listesi

Kullanıcı listeyi kademe kademe verecek. Her madde: risk seviyesi, bağımlılıklar, hangi fazdan sonra yapılabileceği.

| ID | Özellik | Risk | Bağımlılık | Not |
|---|---|---|---|---|
| F-1 | **Çoklu client sınırı**: kişi başı 3 client; 1–2 drop alır, 3. sadece exp, 4+ engellenir | **Yüksek** (drop/ekonomi + güvenlik) | Faz 1 (monitoring), Faz 2 (regresyon listesi) | Tasarım soruları: "kişi" nasıl tanınacak? Client'tan gelen donanım kimliği (HWID) taklit edilebilir (client'a güvenilmez); IP tek başına yetmez (Türkiye'de CGNAT, internet kafeler, aynı evde birden fazla oyuncu). 1./2./3. client sırası neye göre belirlenir, çıkış/yeniden girişte nasıl değişir? Parti exp paylaşımı, drop'un hangi noktada engelleneceği (yerdeki item, sahiplik), quest ödülleri. Kodlamadan önce etki analizi ve onay |

---

## Açık konular

Kanıt durumu: **Kanıtlı** = koddan/ortamdan doğrulandı; **Kısmen**; **Doğrulanmadı**.

| ID | Konu | Önem | Kanıt | Faz |
|---|---|---|---|---|
| K-1 | P2P soketi public IP'ye bağlanıyor (`server-src/src/game/main.cpp:555`, internal IP seçeneği yorum satırı); gelen P2P bağlantıları kontrolsüz kabul ediliyor (`desc_manager.cpp:108-134`). Çekirdekler bu kanala güveniyor: ör. `GG::SHUTDOWN` alan çekirdek hiçbir kontrol yapmadan 10 sn içinde kapanıyor (`input_p2p.cpp:470-474`). **Test VM'de `pf` ile dışarıya kapatıldı (2026-10-05); kodda hâlâ kimlik doğrulaması yok, production'da firewall zorunlu** | **Kritik** (production'da) | Kanıtlı; düzeltme VM'de doğrulandı | 1.1 |
| K-2 | Hesap şifreleri `*`+SHA1(SHA1) (tuzsuz) saklanıyor; giriş, SQL'de `PASSWORD('<şifre>')` ile özet üretip `strcmp` ile karşılaştırıyor (`server-src/src/game/input_auth.cpp:268`, `game/db.cpp:363`). **Metin2 standardı** (7 kaynağın hepsi aynı biçim). Ağda şifreli (`game/SecureCipher.cpp`), çevrimiçi tahmin sınırlı (`input_auth.cpp:15-16`). Zayıflıklar: (1) DB sızarsa hızlı kırılabilir; (2) `PASSWORD()` MySQL 8.0'da kaldırıldı, MariaDB'de `old_passwords`'e bağlı ve "uygulamalar için değil" (resmi belgeler); (3) şifre SQL metninde açık → genel sorgu log'u açılırsa yazılır. Tuzak: MariaDB connector'ındaki `ma_make_scrambled_password` **eski 16 karakterlik** biçimi üretir (`vendor/mariadb-connector-c-3.4.5/libmariadb/ma_password.c:126-130`) — topluluk düzeltmesinin (`@fixme138`) birebir kopyası girişleri bozar | Orta (önceki "Kritik" fazla sertti) | Kanıtlı | 1.2 |
| K-3 | Yönetim kanalı: (1) repodaki şifre upstream'in herkese açık reposundaki değerle aynıydı; config'te yoksa kod varsayılanı da kaynakta açık (`server-src/src/game/config.cpp:78`). (2) Gelen her metin komutu olduğu gibi her zaman açık seviyede log'a yazılıyor (`game/input.cpp:238`) → yönetim girişi yapıldığında şifre `syslog.log`'a düz metin düşer. `config.cpp:198` da şifreyi yazar ama seviye 1'de; şu anki log seviyesinde yazılmıyor (VM'de 0 satır). (3) Erişim `ADMINPAGE_IP` listesiyle sınırlı; **liste boşsa şifreyi bilen herkes yönetici olur** (`input.cpp:250-268`). **Test VM'de config adımı yapıldı (2026-10-05): rastgele şifre, liste `127.0.0.1`, repoda yer tutucu; log'a düşme kodda hâlâ açık** | Yüksek | Kanıtlı | 1.3 |
| A-1 | **Client'ta hile koruması yok** (upstream `63879e03` HackShield/XTrap kaldırdı). **Sunucuda 3 hız/kombo hilesi tespiti var, ama bu yapılandırmada hiçbiri oyuncuyu atmıyor veya engellemiyor, sadece logluyor:** (1) zaman hilesi: atma satırları yorumda (`server-src/src/game/input_main.cpp:1615-1622`); (2) saldırı hızı sayacı, her 60 sn'de bir, eşik `SPEEDHACK_LIMIT_COUNT: 300` (`server/share/conf/game.txt:14`): `log.speed_hack`'e yazar, bağlantıyı sadece `!LC_IsEurope()` iken keser (`game/char.cpp:7077-7090`); `common.locale = english` → `LC_ENGLISH` Avrupa listesinde (`game/locale_service.cpp:1342-1374`), `turkey` de listede → kesme hiç çalışmaz; (3) kombo hilesi: `CHECK_MULTIHACK: 0` (`server/share/conf/game.txt:11`). Saldırı hızı ihlalinde saldırının ayrıca reddedilip reddedilmediği (`game/battle.cpp:822`) ve mesafe/miktar/sahiplik kontrolleri incelenmedi. `log.speed_hack` tablosu monitoring için hazır sinyal (şu an 0 kayıt) | Yüksek | Kanıtlı (3 mekanizmanın durumu); diğer sunucu kontrolleri incelenmedi | 1.5 (monitoring), 2, her özellik |
| A-2 | Kişisel veri: log tablolarında oyuncu IP'leri (`log.cpp:280`). Saklama süresi, silme, KVKK/GDPR | Orta | Kısmen | 1 |
| A-3 | DDoS / ağ koruması; `ENABLE_PROXY_IP` flag'inin işlevi (`common/service.h:5`) | Yüksek | Doğrulanmadı | açılış öncesi |
| A-4 | Client dağıtımı: launcher/patcher, versiyon kontrolü (`config.cpp:61`), kod imzalama | Orta | Kısmen | açılış öncesi |
| A-5 | `.pck` paketlerinin korunması (açılabilirlik) | Orta | Doğrulanmadı | — |
| A-6 | Sunucu quest metinleri sadece İngilizce (`server/share/locale/english`) | Orta | Kanıtlı | açılış tanımına bağlı |
| A-7 | Hukuki durum: Metin2 ve client asset'leri Webzen/Gameforge'un fikri mülkiyeti | Proje düzeyi | — | kullanıcı kararı |
| A-8 | Upstream'den ayrışma: değişiklikleri izole tut, upstream'e dokunanları işaretle | Orta | — | sürekli |
| A-9 | ✅ **Derleme süreci doğrulandı (2026-10-05)**: sıfırdan yapılandırma + derleme 0 hatayla geçti, komutlar `docs/build-and-run.md` → "Server binary'lerini derleme". Açık kalan: (1) çalışan binary'lerin vendor MariaDB kütüphanesi `-O2 -g`, yeni derleme `-O3` (sebebi kayıtlı değil) → ilk devreye almada DB bağlantısı test edilmeli; (2) production binary'lerinde debug bilgisi kararı (1.7 ile) | Düşük | Kanıtlı | ilk binary devreye alma |
| A-11 | Oyun portundaki metin komut kanalında bazı komutlar yönetici kontrolünden (`IsAdminMode`) **önce** işleniyor; biri db'ye istek gönderiyor (`server-src/src/game/input.cpp:240-340`). Kanal herkese açık oyun portunda. Etkisi (özellikle veri/ekonomi) ayrı etki analiziyle değerlendirilmeli; komut gönderilerek test edilmedi. **Test VM'de (2026-10-05) `ADMINPAGE_IP` dışındaki IP'ler için kapatıldı:** geri getirilen port güvenliği kontrolü komutları işlemeden reddediyor. Liste içindeki IP'ler için aynı sıra hâlâ geçerli (kodda değişmedi) | Orta (liste dışı için kapandı) | Kanıtlı (kod okuması); kanal komut gönderilerek test edilmedi | Faz 3 yönetim servisi tasarımında |
| A-12 | **Kurulum ağacı ve derleme kaynağı herkese yazılabilir, root çalıştırıyor** (analiz: `docs/engineering/a12-permissions.md`). VM'de `server/` altında 7.227 dosya `0666`, 535 dizin `0777` (`start.py`, `channels.py`, `share/bin/`, `share/data`, `share/locale` dahil); `server-src` 1.582/1.585. `rc.d/m2dev` → root `python3 start.py` → `share/bin/{game,db}`: yerel her hesap kodu değiştirip root olarak çalıştırabilir, oyun verisini (proto/quest/drop) değiştirebilir. **Kök neden:** ağaç Windows checkout'undan arşivle taşındı ve FreeBSD'de root olarak açıldı (modlar korunur); mod deseni Windows `tar.exe` (bsdtar) ile birebir yeniden üretildi; ilk aktarımın aracı kayıtlı değil (Unverified). **`perms.py` bu ağacı açıklamaz** (önceki ifade düzeltildi): sadece `share/bin/{game,db}` ve **`/var/db/mysql` altındaki bütün dosyalara `0777`** uygular — ayrı ve kesinlikle kabul edilmeyen risk (VM'de çalışmamış). `share/conf` 2026-10-05'te `640`/`750` yapıldı. Ayrıca belgelenmemiş giriş hesabı `m2build` (`wheel`, amacı Unverified). Root kodun ihtiyacı değil (ayrıcalıklı çağrı yok, portlar 1024 üstü); kullanıcı değişikliği ayrı tasarım | **Kritik** (production'da) · Yüksek (test VM, host-only ağ) | Kanıtlı (envanter, yeniden üretim, kod); aktarım aracı Unverified | production kurulumu; her deploy (izin doğrulaması) |
| A-13 | Depo (safebox) şifresi veritabanında **açık metin**: `player.safebox.password varchar(6)` (`server/sql/player.sql:1225`) | Düşük–Orta | Kanıtlı (şema); kod yolu incelenmedi | K-2 C ile birlikte değerlendirilir |
| A-10 | **Savaş/etkinlik haritaları bütün kanallar için tek çekirdekte**: `channel99_core1` imparatorluk savaşı (181–183, `castle.cpp:421-423`), OX (113), `t1–t4` (103/105/110/111, adlarından savaş/turnuva haritası olduğu tahmin ediliyor), `sungzi` (114, 118–128) haritalarını yüklüyor (`server/channels.py:8-9`, `locale/english/map/index`). Büyük savaşta yük tek thread'de toplanır | **Yüksek** (hedef: lagsız savaş) | Kanıtlı (harita dağılımı); yük etkisi ölçülmedi | Faz 2 yük testi (G-2); sonuca göre CH99'u birden çok çekirdeğe bölmek değerlendirilir |

## Teknik borç

| Konu | Not |
|---|---|
| Upstream `pack.py --all` paralel çalışınca sessizce başarısız | Şimdilik repo dışı script; upstream'e bildirilebilir ya da `pack.py` düzeltilebilir |
| `start.py` konsola bağlı çocuk süreçler bırakıyor | VM'de `daemon(8)` ile aşıldı; production kurulumunda aynı servis dosyası gerekecek |
| VM'e özel servis ve MariaDB ayarları repoda değil | Production kurulumunda repoya taşınmalı |
| ✅ `client/config.exe` kaynaksız ve bozuktu | Ayarları client'ın okumadığı `metin2.cfg`'ye yazıyordu. Kaynaktan yeniden yazıldı (`client-src/src/Config`); oyunda frekans/MSAA hatası da düzeltildi → `docs/worklog/2026-10-05-config-tool.md` |
| `GAMMA` ayarı oyunda hiç uygulanmıyor (bilerek bırakıldı) | Sadece `CPythonSystem::ApplyConfig` uyguluyor, onu hiçbir şey çağırmıyor (Anka2'de de aynı); sadece tam ekranda etkili ve parlaklığı kırpan bir çarpan. 2026-10-05 kararı: gerek yok, `config.exe`'de yok. Açılacaksa varsayılanı nötr (`2`) yap → `docs/worklog/2026-10-05-config-tool.md` |
| `heart_idle` gecikmede fazladan pulse sayıyor | Geçen pulse'ın üstüne +1 dönüp tabanı "şimdi"ye kaydırıyor (`server-src/src/libthecore/heart.cpp:49-55, 70`); her gecikmede oyun saati ~1 pulse (16,7 ms) öne geçer, ölçülen: 10 sn'de 79 gecikme → 653 pulse (beklenen ~601). Pulse'a bağlı zamanlayıcılar yük altında hızlanır. `libthecore` db ile ortak → etki analiziyle, yük testinden önce değerlendirilmeli → `docs/worklog/2026-10-05-server-metrics.md` |
| Oyuncu tabloları Aria (`player.player`, `player_index`, `safebox`…; `server/sql/player.sql`: 29 Aria, 10 InnoDB) | Sonuçları: (1) VM ani kesilince Aria kaydı bozuldu, MariaDB açılmadı (`docs/worklog/2026-10-05-server-metrics.md`); (2) kilitsiz fiziksel sıcak yedek mümkün değil, yedek global okuma kilidiyle alınıyor; kilit item sayısıyla büyüyor: 1,77 milyon item'da 4,3 sn, 3,54 milyonda 8 sn (`docs/backup.md`), production ölçeğinde kabul edilemez; (3) Aria tablo kilidi: bir tabloda uzun okuma o tabloya bütün yazmaları durduruyor (ölçüldü: 6.974 ms). InnoDB'ye geçiş ikisini de çözer ama DB şeması değişikliği: davranış farkları, performans ve migration için ayrı etki analizi gerekir (kodun motora bağımlılığı incelenmedi). Karar bekliyor |
| Client DPI-unaware | `GetProcessDpiAwareness` = 0; ekran ölçeği %125+ olan oyuncularda Windows pencereyi büyütüp bulanıklaştırır (Anka2'de de aynı). Etki analizi gerekli |
