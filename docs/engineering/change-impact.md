# Değişiklik Etki Analizi

Bu bir MMORPG: bir değişiklik sadece dokunduğu yeri değil, bütün oyunu etkileyebilir. Amaç "çalışan kod"
değil; stabilite, performans, güvenlik ve veri bütünlüğünü koruyan değişiklik.

**Kural:** Anlamlı her değişiklikten önce bu analiz yapılır ve sonucu PR açıklamasına eklenir.
Yüksek riskli işlerde sıra: **analiz → kullanıcı onayı → kod → doğrulama (§6).**

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
    Game ana döngüsünde çalışan kod sağlık kaydında kendiliğinden görünür (`docs/monitoring.md`: `event_us`,
    `chr_us`, `io_us`, `hb_us`, `work_max_us`, `iter_gap_max_us`). Sağlık kaydı şu durumlarda güncellenir:
    - `main.cpp` `idle()`/`heartbeat()` değişiyor (upstream güncellemesi dahil) → ölçüm noktaları yeniden kontrol edilir
    - iş ana döngünün dışında (yeni thread, süreç, servis) → kendi sağlığı için ayrı ölçüm
    - özelliğe özel bir sayı gerekiyor → satırın sonuna yeni alan + `docs/monitoring.md` tablosu (alanlar adıyla
      okunur, `schema` sadece mevcut bir alanın anlamı/biçimi değişirse artar)
    - `PASSES_PER_SEC` değişiyor → tur bütçesi (16,7 ms) ve `tools/metrics/m2metrics.py` `STALL_GAP_US` güncellenir
15. **Bu sistem production'da bozulursa nasıl anlaşılacak, hangi kanıt kalacak, Admin Panel/agent bunu güvenle
    görebilecek mi?** Hedef sadece "çalışmıyor" bilgisi değil, nedeni hızlı ayırabilecek kanıt: yeniden başlatmada
    silinmeyen hata kaydı, sürüm kimliği, makinece okunur durum (`docs/monitoring.md` → "Telemetri sözleşmesi"). Kontrol
    gerekiyorsa sadece önceden tanımlı ve yetkili bir işlem olarak (`docs/architecture.md` → "Kontrol katmanı ilkeleri").
    Bu soru bugünkü işi büyütme gerekçesi değildir: küçük ve doğal sözleşmeler şimdi, entegrasyon ve kontrol Faz 3'te.

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

## 6. Doğrulama (değişiklikten sonra)

§2 değişiklikten **önce** düşündürür; bu bölüm değişiklik yapıldıktan **sonra** uygulanır. Derinlik §1'e göre: **Düşük**'te
bir cümle ("neyi kontrol ettim") yeter; **Orta**'da ilgili maddeler; **Yüksek**'te hepsi.

1. **Kendi çözümünü çürütmeye çalış.** Değişikliği kendin red-team et: yan etki, yeni hata yolu, davranış değişikliği, yanlış
   varsayım. Şu soruyu cevapla: "Bu değişiklik hangi durumda yanlış sonuç üretir ya da bir hatayı başarı gibi gösterir?"
   Kanıt çözümle çelişirse mevcut yaklaşımı savunma; daralt, tasarımı değiştir ya da geri çek. Commit'lenmemiş kendi
   değişikliğini geri almak serbesttir; commit'lenmiş ya da push'lanmış bir şeyi geri almak `AGENTS.md`'deki "Önce sor"a tabidir.
2. **Test hatası ile ürün hatasını ayır.** Bir test ya da reproducer başarısızsa veya tekrarlarda farklı sonuç veriyorsa, sonucu
   doğrudan ürüne yükleme. Önce testin kendisini doğrula: ölçüm sırası, fixture/harness, eşzamanlılık, ortam. Sonra aynı
   senaryoyu yeniden üret. Ürüne bağlamadan önce mümkünse aynı senaryoyu eski sürümde de koş; orada da oluyorsa sebep yeni
   değişiklik değildir.
3. **Hata yolunu düzeltirken başarı yolunu koruduğunu kanıtla.** Yüksek riskte, mümkün olan her yerde aynı ya da eşdeğer
   senaryoyu değişiklik öncesi ve sonrası koş. Etkilenmemesi gereken doğru davranışın (başarılı işlem, dönen alanlar, sıra,
   performans) aynı kaldığını göster. Hedef yalnız yeni testin PASS olması değildir.
4. **PASS tek başına yeterli değildir.** Her test için neyi kanıtladığını, neyi kanıtlamadığını ve kalan bilinmeyenleri yaz.
   Testin hedeflediği durumu gerçekten oluşturduğunu kontrol et (taşma testi taşma üretti mi, hata enjeksiyonu hatayı
   tetikledi mi). Yeni davranış baseline'dan farklıysa farkın beklenen ve gerekçeli olduğunu kanıtla; açıklanamayan fark
   varken işi bitmiş sayma.

**Örnek (PR #22, AsyncSQL 2a):**
- Red-team ikinci bir boşluk buldu: satırları okunamayan SELECT "uygulandı" görünüyordu.
- Bir sıra değişikliğinin yan etkisi görüldü ve değişiklik daraltıldı.
- Bir çökme testin kendi hatasıydı; ~1 sn'lik takılma yeni koda değil, işletim sisteminin RST sınırına aitti. Eski sürümde de
  aynı ölçüldü.
- Başarılı ifadelerin sonuçları eski kütüphaneyle bayt bayt karşılaştırıldı. Eşli A/B ölçümü, bir struct alanının üretici
  maliyetini yakaladı.
- Bir taşma testi ilk denemede PASS verdi ama taşma üretmemişti.

---

## 7. Regresyon ailesi seçimi

Gerçek client senaryoları `docs/engineering/regression-baseline.md`'de (RB-01…RB-15). Her PR'da hepsi koşulmaz: değişikliğin
dokunduğu **garantiler** belirlenir ve yalnız ilgili aile zorunlu olur. Aile seçilmezse PR'da `Etkilenmiyor — <neden>` yazılır.

| Değişiklik türü (örnek) | Etkilenebilecek garanti | Zorunlu senaryolar |
|---|---|---|
| Ticaret, offline shop / pazar, NPC mağazası, depo | item sahipliği, yang korunumu, çift/kayıp item, tekrar giriş kalıcılığı, kesintide davranış | RB-04, RB-05, RB-06, RB-12 (+ RB-07 DB'ye yeni yazma ekliyorsa) |
| Uzaktan NPC / yeni item veya yang işlemi | sunucu tarafı doğrulama, sahiplik, kayıt | RB-04 ya da RB-05 (hangisi aynı yolu kullanıyorsa), RB-06, RB-09/RB-10 |
| Teleport / warp, dungeon / instance | çekirdek-harita geçişi, yeniden bağlanma, tekrar giriş, yeniden başlatma, durum kalıcılığı | RB-01, RB-06, RB-11, RB-13 |
| Karakter / hesap | sahiplik, `player_index`, oluşturma/yükleme | RB-01, RB-02, RB-03, RB-06 |
| Karakter adı (isim değiştirme, ad kontrolü) | ad tutarlılığı (liste/oyun/DB), item ve bekleme süresinin yalnız başarıda tüketilmesi | RB-01, RB-06, RB-15 |
| Ekonomi / item (drop, craft, kullanım) | kayıt, çift/kayıp, kalıcılık | RB-06, RB-09, RB-10 (+ ticaret/depo yolu değiştiyse RB-04/RB-05) |
| SQL katmanı / db önbelleği / AsyncSQL | kesinti ve yeniden bağlanma, sıra (FIFO), kapanış, log güvenliği | RB-03, RB-07…RB-14 + `tools/sql-reliability` (S1–S12, sqlprobe) |
| Log / telemetri | gizlilik, defter tutarlılığı | RB-14 |

Seçim gerekçesi etki analizine yazılır ("hangi garanti, neden bu senaryolar"). Yeni bir garanti ortaya çıkarsa senaryo
`regression-baseline.md`'ye eklenir; tek bir koşudan gelen sayılar eşik olarak yazılmaz.

---

## PR'a eklenecek kısa biçim

Amaç formu doldurmak değil, **bilinmeyenleri görünür kılmak.** Analiz yapılmadan doldurulmuş form, hiç doldurulmamış formdan daha kötüdür.

**Ne kadar doldurulur (risk seviyesine göre, §1):**
- **Düşük:** sadece ilk satır (`Risk: Düşük`) ve 1–3 satır özet. Alanların hepsini doldurma.
- **Orta:** ilgili alanları doldur; ilgisizleri tek satırda `Etkilenmiyor — <neden>` yap.
- **Yüksek:** bütün alanlar.
- **Doğrulama alanları (§6):** `Başarı yolu önce/sonra` yüksek riskte zorunludur. `Kanıtlamadığı / kalan bilinmeyen` orta ve
  yüksek riskte zorunludur.
- **Regresyon ailesi (§7):** orta ve yüksek riskte zorunludur (seçilen senaryolar ya da `Etkilenmiyor — <neden>`).

**Her alan şu üç cevaptan biri olmalı. Boş bırakılmaz, tahminle doldurulmaz:**
1. **Kanıtlı cevap:** kısa cevap + kanıt (`yol:satır`, log satırı, test sonucu).
2. **`Etkilenmiyor — <neden>`:** neden zorunlu. Tek başına "yok" ya da "N/A" geçersiz, çünkü "düşünüldü mü, atlandı mı" anlaşılmaz.
3. **`Bilinmiyor — <neyi kontrol ettim; nasıl öğrenilir>`.** "Sorun olmaz" gibi kanıtsız bir cümle yazmaktan her zaman iyidir.

**`Bilinmiyor` kuralı:** Yüksek riskli işte `Regresyon`, `Crash`, `Yük`, `Güvenlik`, `Ekonomi/dupe`, `Migration/uyum`, `Rollback` ya da
`Test` alanlarından biri `Bilinmiyor` ise önce araştır; araştırmayla kapanmıyorsa kullanıcıya sor. **`Bilinmiyor` ile koda geçme.**
Düşük/Orta riskte `Bilinmiyor` PR'da açıkça kalabilir.

```text
Etki analizi — Risk: Düşük | Orta | Yüksek
Etkilenen: …            Bağlı sistemler: …
Regresyon riski: …      Crash riski: …
Yük (CPU/RAM/ağ/DB): …  Güvenlik/exploit: …
Ekonomi/dupe: …         Migration: … / Eski veriyle uyum: …
Rollback: …             Test: …
Monitoring/log: …       Panel/agent teşhisi: …
Doküman: …              Teknik borç: …
Alternatif değerlendirildi mi: …
Başarı yolu önce/sonra: …   Kanıtlamadığı / kalan bilinmeyen: …
Regresyon ailesi (§7): …
```

**Örnek** (PR #5, P2P firewall; üç cevap biçimi de görünüyor; §6 alanlarından önce yazıldı):

```text
Etki analizi — Risk: Yüksek
Etkilenen: runtime ağ, P2P portları 12000–12999 (deploy/freebsd/pf.conf); oyun/db kodu değişmedi
Bağlı sistemler: çekirdekler arası mesajlar (input_p2p.cpp), transfer/notice
Regresyon riski: P2P bozulabilirdi → yeniden başlatmada 4 çekirdek arası tam mesh kuruldu, /transfer log'dan doğrulandı
Crash riski: Etkilenmiyor — kod değişmedi, sadece paket filtresi
Yük (CPU/RAM/ağ/DB): Bilinmiyor — gelen her paket tek kurala karşı değerlendirilir; yük altında ölçülmedi (Faz 2 yük testi). Kullanıcı bilerek onayladı
Güvenlik/exploit: K-1 kapandı (VM); kodda kimlik doğrulaması hâlâ yok → production'da kural zorunlu
Ekonomi/dupe: Etkilenmiyor — item/yang akışına dokunulmadı
Migration: Gerekmiyor / Eski veriyle uyum: Etkilenmiyor — veri değişmedi
Rollback: `pfctl -d` (anında); kural dosyası repoda
Test: dışarıdan TCP ölçümü, VM yeniden başlatma, iki client'lı oyun testi
Monitoring/log: `pfctl -vsr` sayacı; Bilinmiyor — engellenen denemelerin log'a yazılması değerlendirilmedi (roadmap 1.5)
Panel: Etkilenmiyor — panel henüz yok (Faz 3); engellenen deneme sayısı ileride aday sinyal
Doküman: güncellendi (production-checklist, worklog, architecture)   Teknik borç: kural production'da ayrıca kurulmalı
Alternatif değerlendirildi mi: evet — iç IP'ye bağlama reddedildi (db public IP'yi dağıtıyor)
```
