# Değişiklik Etki Analizi

Bu bir MMORPG: bir değişiklik sadece dokunduğu yeri değil, bütün oyunu etkileyebilir. Amaç "çalışan kod"
değil; stabilite, performans, güvenlik ve veri bütünlüğünü koruyan değişiklik.

**Kural:** Anlamlı her değişiklikten önce bu analiz yapılır ve sonucu PR açıklamasına eklenir.
Yüksek riskli işlerde sıra: **analiz → kullanıcı onayı → kod.**

Analizin cevapları kanıta dayanır (`yol:satır`, log, test). Bilinmeyen cevap "bilinmiyor" yazılır, tahmin edilmez.

---

## 1. Risk seviyesi

| Seviye | Örnekler | Gereken derinlik |
|---|---|---|
| **Düşük** | UI metni, yazım, görsel ayar, sadece doküman | 2–3 satır özet: ne değişti, neyi etkileyebilir |
| **Orta** | Quest mantığı, denge değerleri, UI davranışı, yeni bağımsız komut | §2'deki ilgili sorular + test planı |
| **Yüksek** | Paket/packet yapısı, DB şeması, item/yang akışı (ticaret, depo, drop, craft, mağaza), çekirdekler arası (P2P, warp), db önbelleği, auth/hesap, güvenlik, performans kritik döngüler | §2'deki **bütün** sorular + rollback planı + test planı + onay **kod yazılmadan önce** |

Emin değilsen bir üst seviyeyi seç.

---

## 2. Sorular

**Kapsam ve bağımlılık**
1. Bu değişiklik tam olarak hangi sistemi etkiliyor? (dosyalar, katmanlar: client C++ / Python UI / game / db / quest / proto / config)
2. Bu sisteme bağlı başka hangi sistemler var? (çağıranlar, paketler, quest'ler, DB tabloları, panel/log)
3. Değişiklik başka bir sistemi bozabilir mi?
4. Yeni bir bug ya da regresyon oluşturabilir mi? Hangi mevcut davranış değişiyor?

**Stabilite ve yük**
5. Client crash ya da server/core crash oluşturabilir mi? (null pointer, sınır dışı erişim, beklenmeyen paket)
6. Yüksek oyuncu sayısında davranışı değişir mi? (oyuncu başına döngü, herkese yayın, harita başına iş)
7. CPU, RAM, network ya da veritabanı yükünü artırır mı? Ne kadar, hangi sıklıkla?

**Güvenlik ve ekonomi**
8. Exploit ya da güvenlik açığı oluşturabilir mi? **Client yalan söylerse ne olur?** (her kritik kontrol sunucuda)
9. Item/yang/ekonomide dupe ya da tutarsızlık oluşturabilir mi? (warp, çekirdek değişimi, ticaret, depo, db önbelleği zamanlaması)

**Veri**
10. Veritabanı migration'ı gerekiyor mu? Migration SQL'i PR'da var mı?
11. Eski oyuncu verileriyle uyumlu mu? (mevcut item'lar, karakterler, kayıtlar)
12. Geri almak gerekirse rollback yöntemi var mı? (kod geri alma + veri geri alma + yedek)

**Doğrulama ve işletim**
13. Nasıl test edilecek? (oyun içi senaryo, log kontrolü, regresyon listesi, yük)
14. Monitoring ya da log eklemek gerekiyor mu? Sorun çıkarsa nasıl fark edeceğiz?
15. Admin Panel / yönetim servisi tarafında kontrol ya da gözlem gerektiriyor mu? (ayar, komut, log)

**Uzun vade**
16. Dokümantasyon güncellenmeli mi? (`docs/*`, worklog)
17. Uzun vadede teknik borç oluşturuyor mu? Upstream'den ayrışmayı artırıyor mu?
18. Mevcut mimari içinde yapılması gerçekten doğru mu, yoksa farklı ya da daha güvenli bir yaklaşım var mı?

---

## 3. Metin2'ye özel kontrol noktaları

Bu projenin mimarisinden gelen, sık gözden kaçan noktalar (`docs/architecture.md` → "Çalışma modeli"):

- **Çekirdek tek thread'li.** Bir döngüde yapılan yavaş iş (büyük döngü, senkron sorgu, toplu yayın) bütün çekirdeği ve o çekirdekteki herkesi yavaşlatır.
- **db oyuncu verisini önbellekte tutar** (varsayılan ~7 dk). Oyuncu verisine veritabanından doğrudan yazmak, önbellekteki kopya tarafından ezilebilir. Oyuncu verisi oyun/db yolu üzerinden değiştirilir.
- **Çekirdekler arası işlemler** (warp, P2P mesajları, kanal değişimi) zamanlama ve tekrar riskine en açık yerdir; dupe'lar genelde burada doğar.
- **Client'a güvenilmez.** Upstream client tarafı hile korumasını kaldırdı; hız, mesafe, miktar, sahiplik kontrolleri sunucuda yapılır.
- **Client ↔ server değişiklikleri birlikte dağıtılır.** Paket yapısı değişirse eski client ile yeni server (ya da tersi) uyumsuz kalır.
- **Hazır binary'lere güvenme.** Exe ve server binary'leri kaynaktan derlenir.

---

## 4. Riskli istek kuralı

Bir istek teknik olarak yapılabilir ama stabiliteye, performansa, güvenliğe ya da veri bütünlüğüne zarar verme ihtimali
yüksekse agent kodlamaya başlamaz. Önce şunları sunar: riskler (kanıtla), alternatifler, önerisi. Karar kullanıcınındır.

---

## 5. Kaynak sırası

1. Bu projenin kodu ve verisi (asıl kanıt).
2. Upstream geçmişi, issue'ları, belgeleri.
3. Bilgisayardaki diğer Metin2 kaynakları (destekleyici).
4. Topluluk/forumlar: sadece araştırma. Bulunan çözüm bu projenin koduyla karşılaştırılmadan uygulanmaz.

Genel Metin2 bilgisi kanıt değildir.

---

## PR'a eklenecek kısa biçim

```text
Etki analizi — Risk: Düşük | Orta | Yüksek
Etkilenen: …            Bağlı sistemler: …
Regresyon riski: …      Crash riski: …
Yük (CPU/RAM/ağ/DB): …  Güvenlik/exploit: …
Ekonomi/dupe: …         Migration: … / Eski veriyle uyum: …
Rollback: …             Test: …
Monitoring/log: …       Panel: …
Doküman: …              Teknik borç: …
Alternatif değerlendirildi mi: …
```
