# Roadmap

Son güncelleme: 2026-10-05

Bu dosya sadece bir istek listesi değil: fazların sırası, birbirine bağımlılıkları, riskleri ve "bitti" kriterleri.
Açık sorunlar ve teknik borç da burada tutulur. Bir madde tamamlandığında PR/worklog linkiyle işaretlenir.

**Temel ilke:** Yeni özellik eklemeden önce stabilite, performans, güvenlik ve veri bütünlüğü korunur.
Ölçülemeyen şey hakkında karar verilmez.

---

## Açılış tanımı (henüz belirlenmedi)

Roadmap'in önceliklerini bu bölüm belirler. Kullanıcıyla birlikte doldurulacak.

- Hedef eşzamanlı oyuncu sayısı (açılışta / 6 ay sonra): —
- Kanal sayısı ve sunucu donanımı: —
- Açılışta olacak sistemler: —
- Açılışta olmayacak (sonraya kalan) sistemler: —
- Hedef kitle ve dil: —
- Açılmadan önce geçilmesi gereken kalite eşikleri (ör. X saat yük testi çökmesiz, restore testi başarılı): —

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

## Teknik borç

| Konu | Not |
|---|---|
| Upstream `pack.py --all` paralel çalışınca sessizce başarısız | Şimdilik repo dışı script; upstream'e bildirilebilir ya da `pack.py` düzeltilebilir |
| `start.py` konsola bağlı çocuk süreçler bırakıyor | VM'de `daemon(8)` ile aşıldı; production kurulumunda aynı servis dosyası gerekecek |
| VM'e özel servis ve MariaDB ayarları repoda değil | Production kurulumunda repoya taşınmalı |
