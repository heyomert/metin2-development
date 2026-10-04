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
| 1.1 | **P2P portlarını kapat** (K-1) | Kritik. Firewall ve/veya internal IP'ye bağlama; çekirdekler arası iletişim test edilmeli | 12xxx portlarına dışarıdan bağlanılamıyor, warp/çekirdek değişimi çalışıyor |
| 1.2 | **Şifre saklama yöntemini modernleştir** (K-2) | Yüksek risk: auth + hesap tablosu + mevcut şifrelerin geçişi. Etki analizi şart | Yeni hesaplar modern hash'le, eski hesaplar girişte kayıpsız geçiyor |
| 1.3 | Yönetim kanalı şifresi ve log'a yazılması (K-3) | Şifre config'de zayıf, `config.cpp:198` log'a yazıyor | Güçlü şifre, log'da görünmüyor |
| 1.4 | Log rotasyonu (`newsyslog`) | db her 5 sn'de syslog'a yazıyor | Loglar boyut/süreyle dönüyor, eski loglar arşivleniyor |
| 1.5 | Monitoring | Süreç başına RAM/CPU, döngü süresi, oyuncu sayısı, log boyutu, DB bağlantıları | Zaman serisi grafikleri; bellek artışı görülebiliyor |
| 1.6 | Otomatik DB yedeği + **restore testi** | Yedek sunucu dışında ve şifreli | Restore düzenli test ediliyor |
| 1.7 | Core dump + çökme uyarısı | Sessiz yeniden başlatma yok | Çökmede dump + uyarı; sebebi bulunabiliyor |
| 1.8 | Production config temizliği | `gmhost *.*.*.*`, test hesapları, varsayılan değerler | Kontrol listesi geçti |

### Faz 2 — Test altyapısı
**Bağımlılık:** Faz 1 (monitoring olmadan yük testi sonucu okunamaz).

| # | İş | Not |
|---|---|---|
| 2.1 | Regresyon kontrol listesi | Giriş, karakter seçimi, warp/çekirdek değişimi, ticaret, depo, item kullanma/düşürme, quest, ölüm/diriliş, çıkış türleri |
| 2.2 | Yük testi aracı (bot / headless client) | Zor: paket protokolü modernize edilmiş, şifreli handshake var. "Yüksek oyuncu sayısı" sorusunun tek gerçek cevabı |
| 2.3 | Ekonomi telemetrisi | Yang ve item'ın sisteme giriş/çıkışı; dupe şüphesi tespiti |

### Faz 3 — Yönetim servisi + Admin Panel MVP
**Bağımlılık:** Faz 1 (monitoring verisi panelin ilk ekranları), Faz 1.3 (güvenli yönetim kanalı).
Mimari: `docs/architecture.md` → "Admin Panel mimari kararı".

| # | İş | Not |
|---|---|---|
| 3.1 | Yönetim servisi (backend) | Panelin konuştuğu tek yer; yetki, işlem kaydı, hız sınırı |
| 3.2 | Panel MVP — **sadece okuma** | Sunucu/çekirdek durumu, oyuncu sayısı, hesap/karakter görüntüleme, loglar |
| 3.3 | Kontrollü işlemler | Duyuru, kick, ban/mute, etkinlik, bakım — oyunun komut kanalı üzerinden |
| 3.4 | Çevrimdışı veri işlemleri | Sadece hesap çevrimiçi ve db önbelleğinde değilken, işlem kaydıyla |

Hazır araçlar (ör. monitoring için Grafana/Prometheus) değerlendirilmeli; panel oyuna özgü işlemlere odaklanmalı.

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
| K-1 | P2P soketi public IP'ye bağlanıyor (`server-src/src/game/main.cpp:555`, internal IP seçeneği yorum satırı); gelen P2P bağlantıları kontrolsüz kabul ediliyor (`desc_manager.cpp:108-134`). Çekirdekler bu kanala güveniyor: ör. `GG::SHUTDOWN` alan çekirdek hiçbir kontrol yapmadan 10 sn içinde kapanıyor (`input_p2p.cpp:470-474`) | **Kritik** | Kanıtlı | 1.1 |
| K-2 | Şifreler MySQL `PASSWORD()` ile (tuzsuz, SHA1 tabanlı) kontrol ediliyor (`server-src/src/game/input_auth.cpp:268`). Giriş sorgusunda escape var (`:240-243`) | **Kritik** | Kanıtlı | 1.2 |
| K-3 | Yönetim kanalı şifresi zayıf bir değerle config'de; `config.cpp:198` şifreyi log'a yazıyor (seviye 1). Erişim IP listesiyle sınırlı (`ADMINPAGE_IP`) | Yüksek | Kanıtlı | 1.3 |
| A-1 | Client tarafında hile koruması yok (upstream `63879e03` HackShield/XTrap kaldırdı). Sunucu tarafı kontrollerin yeterliliği incelenmedi | Yüksek | Kısmen | 2 / her özellik |
| A-2 | Kişisel veri: log tablolarında oyuncu IP'leri (`log.cpp:280`). Saklama süresi, silme, KVKK/GDPR | Orta | Kısmen | 1 |
| A-3 | DDoS / ağ koruması; `ENABLE_PROXY_IP` flag'inin işlevi (`common/service.h:5`) | Yüksek | Doğrulanmadı | açılış öncesi |
| A-4 | Client dağıtımı: launcher/patcher, versiyon kontrolü (`config.cpp:61`), kod imzalama | Orta | Kısmen | açılış öncesi |
| A-5 | `.pck` paketlerinin korunması (açılabilirlik) | Orta | Doğrulanmadı | — |
| A-6 | Sunucu quest metinleri sadece İngilizce (`server/share/locale/english`) | Orta | Kanıtlı | açılış tanımına bağlı |
| A-7 | Hukuki durum: Metin2 ve client asset'leri Webzen/Gameforge'un fikri mülkiyeti | Proje düzeyi | — | kullanıcı kararı |
| A-8 | Upstream'den ayrışma: değişiklikleri izole tut, upstream'e dokunanları işaretle | Orta | — | sürekli |
| A-9 | Server binary'lerinin derleme komutu belgelenmedi | Düşük | Doğrulanmadı | ilk server C++ değişikliği |
| A-10 | **Savaş/etkinlik haritaları bütün kanallar için tek çekirdekte**: `channel99_core1` imparatorluk savaşı (181–183, `castle.cpp:421-423`), OX (113), `t1–t4` (103/105/110/111, adlarından savaş/turnuva haritası olduğu tahmin ediliyor), `sungzi` (114, 118–128) haritalarını yüklüyor (`server/channels.py:8-9`, `locale/english/map/index`). Büyük savaşta yük tek thread'de toplanır | **Yüksek** (hedef: lagsız savaş) | Kanıtlı (harita dağılımı); yük etkisi ölçülmedi | Faz 2 yük testi (G-2); sonuca göre CH99'u birden çok çekirdeğe bölmek değerlendirilir |

## Teknik borç

| Konu | Not |
|---|---|
| Upstream `pack.py --all` paralel çalışınca sessizce başarısız | Şimdilik repo dışı script; upstream'e bildirilebilir ya da `pack.py` düzeltilebilir |
| `start.py` konsola bağlı çocuk süreçler bırakıyor | VM'de `daemon(8)` ile aşıldı; production kurulumunda aynı servis dosyası gerekecek |
| VM'e özel servis ve MariaDB ayarları repoda değil | Production kurulumunda repoya taşınmalı |
