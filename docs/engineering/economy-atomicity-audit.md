# Ekonomi atomikliği ve bağlantı kopması güvenliği denetimi (A-27)

Durum (2026-10-10): **açık, başlanmadı.** Kaynak: A-19 (depo aktivasyonu) kabulünde canlı gözlenen kalan risk
(`docs/engineering/safebox-activation.md` bölüm 5). Bu doküman yalnızca yöntemi ve aday listesini tutar; **hiçbir sistemde
benzer bir riskin olduğu varsayılmaz.** Her satır gerçek kod yolu kanıtıyla (`yol:satır`) doldurulur.

## Soru

Oyuncu ödeme, item tüketimi ve sonuç üretimi arasındaki kritik anda logout, disconnect, çekirdek kaybı, game çökmesi ya da
DB belirsizliği yaşarsa: ücretsiz sonuç, çift ücret, item kaybı ya da kopya oluşabilecek başka akış var mı?

## Neden olabilir (genel mekanizma, depo için kanıtlı)

- Oyuncunun gold'u ve envanteri game belleğinde; kayıt `GD::PLAYER_SAVE` → db önbelleği, cevapsız ve gecikmeli
  (`server-src/src/db/ClientManagerPlayer.cpp:797-803`). Item kaydı da db önbelleği üzerinden.
- Bir sonuç başka bir yoldan (doğrudan SQL, ayrı async callback, başka çekirdek) yazılıyorsa iki tarafın ortak commit'i yoktur.
- Bu, her ekonomik akışta risk olduğu anlamına **gelmez**: aynı game nesnesinde ve aynı kayıt yolunda biten akışlar tek
  taraflıdır. Hangi akışın iki taraflı olduğu ancak kodla bulunur.

## Her akış için sınıflandırma

| # | Soru |
|---|---|
| 1 | Ekonomik girdi nedir (gold, item, cash, puan)? |
| 2 | Sonuç nedir (item, gold, durum, satır)? |
| 3 | Girdi bellekte ne zaman değişiyor (`yol:satır`)? |
| 4 | DB'ye hangi yolla yazılıyor (önbellek, doğrudan SQL, log)? |
| 5 | Sonuç başka bir SQL ya da async callback'e bağlı mı? |
| 6 | Girdi ve sonuç aynı transaction'da mı? |
| 7 | Tekrar deneme / idempotency var mı? |
| 8 | Logout / disconnect sırasında sahiplik hangi nesnede? |
| 9 | Çökme penceresinde olası sonuç: güvenli / oyuncu kaybı / sunucu kaybı (ücretsiz sonuç) / çift ücret / kopya / bilinmiyor |
| 10 | Canlı test ya da probe gerekiyor mu? |

## Aday yüzeyler (kanıtlanmadı; denetim sırası)

refine / item yükseltme; efsun / bonus; cube ve benzeri üretim; NPC mağazası al/sat; oyuncu ticareti; lonca ekonomisi;
item + yang tüketen quest işlemleri; mall/depo dışındaki depolama ve ekonomi yolları; async SQL sonucu ile oyuncu gold/item
kaydının farklı yollardan ilerlediği her sistem.

## Sonuçlar

| Akış | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Depo aktivasyonu (A-19) | 500 yang | hesap `safebox` satırı | istekte (blokaj) | gold: önbellek; satır: doğrudan SQL | evet (`DG::SAFEBOX_ACTIVATE_RESULT`) | hayır | ODKU idempotent | blokaj oyuncunun kaydında | sunucu kaybı, hesap başına bir kez (kabul edildi) | yapıldı (2026-10-10) |
