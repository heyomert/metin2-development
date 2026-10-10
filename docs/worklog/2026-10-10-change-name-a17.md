# İsim değiştirme DB hatasında item'ı siliyordu (A-17 g19 + g18 DB hatası kısmı)

- **Tarih:** 2026-10-10
- **Tür:** düzeltme / karar
- **Alan:** server-src / server
- **Durum:** Aktif
- **PR / commit:** `fix/a17-change-name` (PR açılınca link)

## Problem / hedef
"Tincture of the Name" (71055) DB hatası anına denk gelirse oyuncu "You have changed your name successfully" görüyordu.
Gerçekte ad değişmiyor, item siliniyor, messenger listesi gidiyor, log "SUCCESS" yazıyor ve 15 günlük bekleme süresi
başlıyordu. Denetimde g19 (UPDATE sonucu okunmuyor) ve g18 (ad kontrolü SELECT'inin hatası "ad boş" sayılıyor) olarak
kayıtlıydı (`docs/engineering/db-step2a-directquery-audit.md`).

## Kök neden / kanıt
- `game/questlua_pc.cpp` `pc_change_name` (düzeltme öncesi 2117-2152): ad kontrolü yalnız `uiNumRows > 0` iken okunuyordu.
  Messenger silme (`RemoveAllList`, asenkron DELETE) ve `ChangeNameLog` (asenkron INSERT) UPDATE'ten **önce** gönderiliyordu.
  `DirectQuery` UPDATE'inin sonucu hiç okunmadan `SetNewName` + 4 dönüyordu. Quest 4'te item'ı siliyor ve `next_time`
  yazıyor (`change_name.quest:82-91`).
- **Test VM uçtan uca (eski binary `71f346d85`, gerçek client, `outage.sh` 95 sn, kanıt
  `/root/acceptance/a17-change-name-20261010T185040Z`):**
  - client: başarı mesajı, item envanterden gitti;
  - hata defteri: `select.player` ve `update.player` `errno=2002 result=not_delivered`;
  - DB: `player.name` değişmedi; `player.item`'da 71055 satırı yok (item önbellekte doğup önbellekte silindi, geri gelmez);
  - messenger fixture'ı (iki yönlü) silindi: asenkron DELETE `retrying` → DB dönünce `recovered` ile uygulandı;
  - `log.change_name` ve `log.log` "SUCCESS: from SbA4 to YeniAd1" yazdı; `next_time` 15 gün sonrasına ayarlandı.
- **Ad üç yerde farklı görünüyordu:** ilk relog'da karakter listesi eski adı (listeyi DB'den okuyor,
  `db/ClientManagerLogin.cpp:182`), oyun yeni adı gösterdi (karakter db önbelleğinden yüklendi; önbellekteki ad
  `CreatePlayerProto`'da `GetNewName`'den geliyor, `game/char.cpp:1187-1194`). Kayıt sorgusu adı DB'ye yazmıyor; çıkıştan
  10 dk sonra (`g_iLogoutSeconds`, `db/Main.cpp:34`) önbellek boşalınca oyunda da eski ad geri geldi. Aynı sürede seviye
  (35) DB'ye yazıldı, ad yazılmadı.
- 2a sonrası `DirectQuery` DB'ye ulaşamazsa hemen hata döndürüyor (audit D1/D2), asenkron sorgular ise yerinde tekrar
  deneniyor. Hatanın görünür olmasının nedeni bu ikisinin birleşimi: senkron UPDATE kayboldu, ondan önce gönderilen
  asenkron yan etkiler DB dönünce uygulandı.

## Reddedilen yaklaşımlar
- **İsim değiştirmeyi db sürecine taşımak (GD/DG paketi, Seçenek B):** ana döngüden senkron sorguyu kaldırır, db
  önbelleğindeki adı da günceller; ama yeni paket yapısı ve asenkron quest akışı gerekir. A-28 ile (UNIQUE index) şema
  adımına ertelendi.
- **Karakter seçimindeki isim değiştirme akışı (`change_name=1`, Seçenek C):** oyuncu deneyimini değiştirir; o yolda da UPDATE
  sonucu okunmuyor (d16) ve bayrağı yazan kod yok.
- **AMBIGUOUS'ta item'ı tüketmek:** UPDATE uygulanmamışsa item kaybı geri gelir. Oyuncu lehine karar: AMBIGUOUS da dönüş 6;
  kabul edilen en kötü durum bir kez ücretsiz isim değişikliği.

## Çözüm
- `game/change_name_result.h`: saf karar fonksiyonları (`AfterNameCheck`, `AfterUpdate`); yeni dönüş
  `RET_DB_ERROR = 6`. Sadece `APPLIED` ve tam 1 satır değişen UPDATE başarıdır.
- `pc_change_name`: ad kontrolü `SQLReadFirstInt` ile; hata → 6. UPDATE başarısızsa 6. Messenger silme,
  `ChangeNameLog` ve `SetNewName` yalnız başarıdan sonra. Hata yolunda syserr `CHANGE_NAME: failed pid=… step=check|update
  result=… errno=…` (ad yok).
- `change_name.quest`: 6 için "could not be changed / item was not used" mesajı ve `char_log("DB ERROR")`; item ve
  `next_time` dokunulmaz. Eski quest yeni game ile de güvenli: bilinmeyen dönüş `else` dalında item'ı silmiyor.
- **Kalan risk (kabul edildi, A-27):** UPDATE commit olduktan sonra, quest item'ı ve bekleme süresini yazmadan önce game
  çökerse bir kez ücretsiz isim değişikliği. Düzeltme kaybı kapatır, işlemi atomik yapmaz.

## Doğrulama
- `tools/change-name/test-change-name-logic.cpp`: 13/13 (VM, FreeBSD clang). Dört mutant (kontrol hatasını yok sayma,
  `bApplied`'ı ya da satır sayısını atlama, sayım eşiğini kaydırma) her biri 1–3 FAIL ile yakalandı.
- Test VM kabulü (RB-15 + RB-01, RB-06, RB-10, RB-14): kurulumdan sonra eklenecek.

## Bir dahaki sefere tuzaklar
- **Oyunda görünen ad kanıt değildir.** Önbellekten yüklenen karakter DB'de olmayan bir adı gösterebilir. Adı DB'den
  doğrula ve önbellek boşaldıktan sonra (çıkış + 10 dk) tekrar gir.
- **Asenkron yan etki ≠ senkron işlem.** 2a'da asenkron sorgu kesintide bekletilip sonra uygulanır, `DirectQuery` hemen
  hata döner. Asenkron yan etkileri senkron sonucun kontrolünden sonraya koy.
- Bu karakterlerde GM yetkisi karakter adına bağlı (`common.gmlist.mName`); bir test karakterine item vermek için geçici
  satır gerekiyor ve yetki yeniden başlatmaya kadar bellekte kalıyor.
