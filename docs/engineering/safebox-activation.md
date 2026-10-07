# Depo (safebox): hesap aktivasyonu, ödeme ve erişim (A-19 / A-20 / A-23)

Durum (2026-10-07): **PR-1 (A-23, açma intent'i) uygulanıyor; PR-2 (hesap aktivasyonu + ödeme) tasarımı onaylı, kodu
yazılmadı.** Kanıt ve canlı test: `docs/worklog/2026-10-07-safebox-a23.md`. Roadmap: A-19, A-20, A-23 (+ yan bulgular A-24,
A-25).

## 1. Ürün kontratı

- **Depo hesap özelliğidir.** Itemler, parola, premium kapasite ve aktivasyon durumu hesabın bütün karakterleri için ortak.
- **500 yang = hesabın deposunu ilk kez aktive etme ücreti**, hesap başına bir kez. Karakter başına ücret modeli hedef değil.
- **Normal depo NPC ister** (Depocu). **Item Shop Storeroom (mall) ayrı sistem**, envanterden uzaktan açılır ve öyle kalır.
  Uzaktan normal depo kısayolu bugün yok (bölüm 2) ve bu işte eklenmiyor.
- **Karakter yaşam döngüsü (zorunlu kabul maddesi):** A ilk aktivasyonu yapar ve bir kez öder; aynı hesabın B'si ücretsiz
  kullanır; depoya item bırakılır; A silinir → depo ACTIVE kalır, B tekrar ödemez, parola ve itemler aynı; yeni C ACTIVE görür,
  ödemez. Bütün karakterler silinip yeni karakter açılsa bile hesabın `safebox` satırı durdukça ACTIVE. Karakter silme
  aktivasyonu, SAFEBOX/MALL itemlerini ve parolayı silmez.

## 2. Bugünkü durum (kanıtlı)

| Konu | Kanıt |
|---|---|
| Ödeme kapısı sadece quest'te (karakter durumu `stash` start/use); sunucudaki yükleme yolunda aktivasyon kontrolü yok | `warehouse.quest:9-20`; `char.cpp` `ReqSafeboxLoad`; canlı test (worklog) |
| `game.open_mall()` normal deponun açma konumunu da ayarlıyordu; mall yüklemesi bu konumu hiç kullanmıyor | `questlua_game.cpp:84-91` (PR-1 öncesi); `cmd_general.cpp` `do_mall_password` |
| Açma konumu kalıcıydı (sadece karakter nesnesi yeniden kurulunca sıfırlanıyordu) | `char.cpp` `Initialize()` |
| Satır olmadan depo tam çalışıyor (parola 000000, itemler `owner_id = hesap id`) | db `RESULT_SAFEBOX_LOAD`; `item_manager.cpp:461`; canlı test |
| game kapasiteyi DB'den almıyor: 1 sayfa, premium/unique item ile 3 | `input_db.cpp:1111-1134` |
| A-19: ödeme Lua'da DB işleminden önce; koşulsuz INSERT → hesabın satırı varken 1062; iade yolu yok | `warehouse.quest:18-19`; `ClientManager.cpp:788-801` |
| A-20: `RESULT_SAFEBOX_CHANGE_SIZE` `uiNumRows > 0` bakıyor, cevap hiç gitmiyor; gitseydi client depo listesini temizleyip 5 slotluk pencere açardı ve sunucuda sayfa/slot birimi karışırdı | `ClientManager.cpp:803-817`; `char.cpp` `ChangeSafeboxSize`; client `PythonSafeBox.cpp:4-15` |
| Envanterdeki buton mall'dur: `/click_mall` → `ShowMeMallPassword` → `/mall_password` → MALL | `uiinventory.py:468-470`; `cmd_general.cpp:1982-1985`; `interfacemodule.py:750-755` |
| `ShowMeSafeboxPassword`'ün tek kaynağı `game.open_safebox` | `questlua_game.cpp:80` |
| Karakter silme: `safebox` satırına dokunmuyor; item silme `window < SAFEBOX(3) OR window = DRAGON_SOUL_INVENTORY(5)` → SAFEBOX/MALL kalır, hesap id = silinen pid çakışmasında da; o pid'nin bütün quest satırları silinir | `ClientManagerPlayer.cpp:1050-1180`; ENUM indeksleri probe ile ölçüldü (worklog) |

## 3. A-23 — normal depo açma intent'i (PR-1)

| | Önce | Sonra |
|---|---|---|
| `game.open_safebox()` | konum ayarlar | konum + **intent** (tek deneme) |
| `game.open_mall()` | depo konumunu da ayarlar | depo durumuna dokunmaz |
| `/safebox_password` | konum ≤1000 ise yükler; konum oturum boyunca kalır | intent yoksa reddeder ("too far" metni); varsa **tüketir**, sonra mevcut kontroller (parola, açık mı, 10 sn, mesafe, üst üste istek) |
| `CloseSafebox` | — | intent'i temizler |
| warp / çekirdek değişimi / logout | yeni nesne → konum sıfır | yeni nesne → intent yok |
| Envanter mall, NPC mall, premium, parola, item yolları, ücret modeli | — | değişmez |

Normal client parola penceresini her denemeden sonra kapatıyor (`uisafebox.py` OnAccept → CloseDialog) ve yeni pencere sadece
NPC'den geliyor; tek kullanımlık intent normal akışı değiştirmez. Değişen tek şey aynı oturumda NPC'siz elle komut yazmak.
Süre sınırı (TTL) yok: tüketim + nesne ömrü + mesafe yeterli, keyfi eşik eklenmedi.

## 4. Hesap aktivasyonu ve ödeme (PR-2 — onaylı tasarım, uygulanmadı)

**Durumlar** (hesap için DB'de; karakter nesnesinde önbellek): `UNKNOWN` (giriş sorgusu dönmedi/hata), `INACTIVE`, `ACTIVE`,
`PENDING` (bu karakterin uçuşta isteği var). **Kaynak:** hesabın `safebox` satırı (PK `account_id`); eski veri kuralı: satır
yok ama SAFEBOX penceresinde item varsa ACTIVE + ücretsiz idempotent satır oluşturma (ensure). Şema değişikliği yok.

**Giriş sorgusu** (game'in mevcut `QID_SAFEBOX_SIZE` sorgusunun yerine, girişteki doğrudan `UPDATE` kalkar):
satır boyutu (yoksa NULL) + `EXISTS(item … owner_id = hesap AND window = 'SAFEBOX')`. Her zaman 1 satır; `owner_id_idx`
kullanılıyor (probe). `errno≠0` → UNKNOWN; UNKNOWN'da ücret yolu açılmaz.

**Aktivasyon ifadesi** (sadece db sürecinde, tek FIFO kuyrukta):
`INSERT … (account_id, size) VALUES(X, 1) ON DUPLICATE KEY UPDATE size = GREATEST(size, 1)` →
`errno=0, affected=1` CREATED; `0/2` ALREADY; `errno≠0` FAILED. Probe (MariaDB 11.8.9, Aria, ReturnQuery): yeni → 1;
aynı → 0; mevcut 3 → 0 (küçülmez); mevcut 0 → 2; parola korunur; yetkisiz kullanıcı → 1142 FAILED; 2 sn kesinti → 2a yerinde
tekrar, tek CREATED; gönderimden sonra bağlantı kesilmesi → 2013 `ambiguous_no_retry` FAILED (o örnekte uygulanmadı), tekrar →
CREATED, sonra ALREADY. `INSERT IGNORE` kullanılmaz (başka hataları gizler).

**Ödeme — blokaj (escrow):** istek anında C++ INACTIVE, bekleyen istek yok ve yang ≥ ücret kontrol eder; ücret düşülür ve
`hold`'a alınır. Kayıtta `gold = GetGold() + hold` (`char.cpp` kayıt tablosu) → blokajdaki para kesinleşene kadar kalıcı
olarak oyuncunundur; taşma kontrolü `hold`'u da sayar (iade asla taşmaya takılmaz). Yeni `GD/DG` paket çifti (A-20'ye
dokunmaz) `hesap + pid + istek kimliği` taşır. CREATED → blokaj kalkar (ücret kesin, money log); ALREADY → iade, ACTIVE;
FAILED → iade, UNKNOWN (yeniden sorgu). Eşleşmeyen ya da oturumu bitmiş sonuç → para işlemi yok, log satırı.
Not: game `MONEY_LOG_QUEST` tipini `money_log`'a yazmıyor (`input_db.cpp:1737`); kesinleşen ücretin izi PR-2'de görünür bir yolla
bırakılmalı.

**Eski veri:** A (satır+item), B (satır) → ACTIVE; C (satırsız+item) → ACTIVE + ensure (itemler korunur); D → INACTIVE;
D' (satırsız, itemsiz, bir karakter eski sistemde ödemiş: quest `use`) → bölüm 6 kapısı.

## 5. ACCEPTED RESIDUAL RISK

Mevcut şema ve gold kalıcılık mimarisi altında aktivasyon satırı ile oyuncunun gold'u **tek atomik commit noktasına
alınamıyor**: gold `GD::PLAYER_SAVE` → `PutPlayerCache` ile cevapsız ve gecikmeli yazılıyor
(`ClientManagerPlayer.cpp:797-803`), satır doğrudan SQL ile; db çevrimiçi oyuncunun gold'unu değiştiremiyor. Bu yüzden:

> **ACCEPTED RESIDUAL RISK:** Hesap başına **en fazla bir kez, 500 yang tutarında ücretsiz aktivasyon** oluşabilir. Kapsam:
> aktivasyon ifadesi uygulanmış ama sonuç karakter/oturum tarafında kesinleştirilememiş (logout, disconnect, warp, çekirdek
> değişimi — dar asynchronous settlement window); belirsiz (ambiguous) SQL sonucu; game/db çökmesi ya da kalıcılık geri sarma
> sınırı. Bu risk item dupe, item kaybı, oyuncunun para kaybı ya da çift ücret **değildir**; hesap başına tekrarlanmaz
> (ACTIVE olduktan sonra tekrar aktivasyon alınmaz). Olaylar **process hayatta kaldığı ve ilgili result path işlendiği
> durumlarda loglanır**; çökme/host arızasında log flush kesin değildir.

Karşılaştırılıp reddedilen daha güçlü modeller: kalıcı PENDING (`size = 0` + settle) aynı pencereyi bir adım kaydırır;
sonraki girişte uzlaşma karakter başına flag gerektirir (başka karakterle ya da silmeyle kaçılabilir; quest kaydı doğrudan SQL,
gold gecikmeli → db çöküşünde yine açık); hesap borcu şema ister ve aynı pencereyi taşır. Ayrı aday (kabul edilmedi,
**Unverified**): Aria'nın host çöküşünde son commit'i koruyup korumadığı (ödendi ama satır kayboldu).

## 6. Kurulum kapısı: D' (production, PR-2 sürümü)

D': `safebox` satırı yok, SAFEBOX itemi yok, eski sistemde ödeme kanıtı sadece bir karakterin quest durumu
(`stash.__status` = derlenmiş `use` indeksi; değeri `quest/object/state/stash` dosyasından okunur, `start` = 0). Karakter
silme bu kanıtı yok eder, bu yüzden sayım zorunlu kapıdır (test VM'deki 0 production kanıtı değildir):

1. game süreçleri ve Metin2 db süreci kontrollü durdurulur (db kapanışta önbelleği boşaltır; sunucu kapalıyken silme olmaz).
2. MariaDB erişilebilir ve stabil kalır.
3. Önbellek boşalmış durumda **salt okunur** D' sayımı.
4. D' = 0 → kurulum devam edebilir.
5. D' > 0 → **STOP**.
6. Önce `m2dev-backup consistent`.
7. Backfill planı ayrıca review ve onay.
8. Onaylı idempotent backfill (bölüm 4'teki ifade, ücretsiz).
9. Tekrar sayım.
10. D' = 0 kanıtı olmadan yeni binary'ler başlatılmaz.

C hesapları için kapı gerekmiyor: item kanıtı karakter silmede korunuyor (bölüm 2), çalışma zamanı kuralı ve ensure yeterli.

## 7. PR sırası

`main` → PR-1 (A-23) → PR-2 (aktivasyon + ödeme, PR-1'in üstünde). **PR-1 tek başına main'e merge edilmez:** tek başına
bypass'ı kapatırken karakter başına ücreti ve 1062'yi zorunlu hale getirir. PR-1 ayrı review/test; tam kabul PR-2 head'inde;
birleşik sonuç PASS olursa art arda merge için ayrıca onay. Main bilerek ara ürün davranışında deploy edilmez.

## 8. Kabul seti

Risk kapsamına göre seçildi (kör Cartesian değil). R1 çift ücret · R2 para kaybı · R3 ücretsiz aktivasyon · R4 yetkisiz
erişim · R5 item kaybı/kopya/sahiplik · R6 eski veride kilitlenme · R7 mall/premium/parola gerilemesi · R8 bayat intent ·
R9 UNKNOWN yanlış sınıflama · R10 A-20'nin istemeden devreye girmesi.

| # | Senaryo | Riskler | PR |
|---|---|---|---|
| S1 | Yeni hesap: yetersiz yang → ret, sonra tam ödeme (500 bir kez, 1062 = 0); INACTIVE'de `/click_mall` sadece mall, elle `/safebox_password` ret | R1 R2 R4 | 2 (elle komut kısmı 1) |
| S2 | Aynı hesabın 2. karakteri ücretsiz, aynı item/parola; yanlış parola → NPC gerekir; relog | R1 R5 R8 | 2 |
| S3 | Ödeyen (seviye < silme sınırı) silinir → diğeri ACTIVE; ödemeyen silinir; hepsi silinir → yeni karakter ücretsiz | R1 R5 R6 | 2 |
| S4 | Satırsız + itemli hesap: ACTIVE, ensure satırı oluşturur, item durur, parola değiştirilebilir | R5 R6 R7 | 2 |
| S5 | Satırlı hesapta ödememiş karakter ücretsiz | R6 | 2 |
| S6 | Mall: NPC ve envanter mall çalışır; mall sonrası `/safebox_password` ret; mall yanlış parola, relog | R4 R7 | 1 |
| S7 | Intent: aç-kapat sonrası komut, aynı haritada tekrar, warp, logout → ret | R8 | 1 |
| S8 | Premium 3 sayfa / normal 1 sayfa | R7 | 1, 2 |
| S9 | Giriş sırasında DB kesintisi → NPC "tekrar deneyin", ücret yok | R9 R1 | 2 |
| S10 | Kesintide istek: (a) biter → tek ücret; (b) beklerken logout → yang korunur, log; (c) beklerken warp/çekirdek değişimi | R2 R3 R1 | 2 |
| S11 | game+db restart → ACTIVE sürer | R6 | 2 |

Canlıda değil, probe/kod ile: belirsiz sonuç, ODKU matrisi, hesap id = silinen pid çakışması (canlıda çakışan karakterler
silme seviye sınırının üstünde), backfill öncesi D' ödeyen silme (kapı testi), GOLD_MAX'a yakın blokaj, bayat sonuç.
RB (`change-impact.md` §7): PR-2 için RB-01, 04, 05, 06, 07, 10, 12, 13, 14; PR-1 ön kabulü için RB-05 + S1 elle komut + S6 + S7.

## 9. Doğrulanmamış

- Aria'nın host çöküşünde kalıcılığı (bölüm 5).
- Quest yeniden derlenince `use` indeksinin aynı kalması (PR-2 kabulünde ölçülecek).
- NPC tıklamasında sunucu tarafı mesafe kontrolü.
- Eski Lua akışında `wait()/select()` arasında yang düşmesiyle ücretsiz aktivasyon (teknik borç; PR-2 bu akışı kaldırır).
