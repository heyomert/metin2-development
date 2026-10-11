# DB adım 2a: `DirectQuery` çağıran denetimi (merge kapısı)

**Durum: denetim + reproducer'lar (2026-10-07); kullanıcı onayıyla merkezi düzeltme ve iki kritik çağıran düzeltmesi 2a'ya
alındı (bölüm 6). Diğer çağıranların iş mantığı değiştirilmedi.** Kaynak yollar `server-src/src/`
altında. Reproducer'lar `tools/sql-reliability/sqlprobe.cpp` D1–D6 (geçici MariaDB; canlı DB'ye dokunulmadı), aynı kod
hem eski (`8fbec589`) hem 2a kütüphanesine karşı derlenip koşuldu, her biri 3/3 (D6 sayıları zamanlamaya bağlı).

## 1. Soru ve cevap
2a'da `DirectQuery` beklemiyor ve tekrar etmiyor; başarısızlık hemen çağırana dönüyor. Soru: bu, çağıranlarda yeni bir
doğruluk (correctness) regresyonu yaratıyor mu?

**Cevap: yeni bir başarısızlık sınıfı yaratmıyor; bugün zaten var olan başarısızlık sınıfını dar bir pencereye daha
genişletiyor. Asıl risk 2a'dan önce de var: başarısız bir `DirectQuery` birçok çağırana başarı gibi görünüyor.**

| Durum | Eski (`8fbec589`) | 2a | Kanıt |
|---|---|---|---|
| D1: DB kapalı, öncesinde yeniden bağlanma yok (genel kesinti) | **hemen hata** (2002), uygulanmadı | aynı | D1, K7c (3/3, eski = 2a birebir) |
| D2: önceki ifade bağlantıyı sessizce yeniden kurmuş, sonra DB kapalı | ilk çağrı **kesinti boyunca donuyor** (game'de ana döngü), DB dönünce uygulanıyor | hemen hata, uygulanmadı | D2, K7b |
| D3: sunucu ifadeyi reddediyor (DB açık) | hata | aynı | D3 |

Eski kodda bekleme yalnız şu durumda vardı: `DirectQuery` girişinde bağlantının thread id'si değişmişse (önceki ifadede
yeniden bağlanma) `while (!QueryLocaleSet());` (`30adca70a:libsql/AsyncSQL.cpp:276-283`). Başarısız bir yeniden bağlanma
thread id'yi değiştirmiyor (D1'de ikinci ve üçüncü çağrı da hemen döndü). Yani "eski kod DB'yi bekleyip uyguluyordu"
**genel davranış değildi**; yalnız D2 penceresinde ve o pencerede de beklemenin üst sınırı yoktu (kesinti sürdükçe bütün
çekirdek donuk).

## 2. Başarısız ifadenin çağırana görünüşü (D3, D4, D5; eski = 2a)
`Store()` başarısızlıkta da bir `SQLResult` üretir (`Get()` hiçbir zaman NULL değil), ama içindeki sayılar yanıltıcı:
- `uiAffectedRows = 4294967295`: Connector her gönderimde `affected_rows = ~0` yapıyor (`vendor/mariadb-connector-c-3.4.5/
  libmariadb/mariadb_lib.c:451`), hata yolunda güncellemiyor; `Store()` `uint32_t`'ye çeviriyor (`libsql/AsyncSQL.h`, `Store`).
  → `uiAffectedRows > 0` ya da `uiAffectedRows <= 0` (uint32'de `== 0`) kontrolü **başarısızlığı başarı sanıyor.**
- `uiInsertID` = **bu bağlantıdaki önceki başarılı INSERT'in id'si** (D4/D5: `equals_earlier_insert=1`).
- `pSQLResult = NULL`, `uiNumRows = 0`: SELECT çağıranları başarısızlığı "satır yok" olarak görür.
- `mysql_fetch_row(NULL)` 0 döner (`mariadb_lib.c:3137-3140`); `row[0]` okuyan çağıran **çöker**.
- **Hiçbir `DirectQuery` çağıranı `uiSQLErrno` okumuyor** (tek okuyan `db/ClientManagerLogin.cpp:137`, ReturnQuery).
- Bazı çağıranlar `(uint32_t)-1`'i zaten kontrol ediyor (upstream'de kısmi düzeltme): `game/guild.cpp:1063`,
  `db/ClientManager.cpp:753, 1799`, `db/ClientManagerPlayer.cpp:1090, 1142`, `db/GuildManager.cpp:1070, 1296`,
  `db/Marriage.cpp:107, 147, 193, 237` (ve ReturnQuery yolunda `game/db.cpp:454`).

**D4/D5 (karakter oluşturma kontrollerinin birebir kopyası, `ClientManagerPlayer.cpp:926-951`):**
- D4 (DB kapalı): INSERT 2002, `insert_check_passes=1`, `player_id` = önceki karakterin id'si, UPDATE 2002 de geçiyor →
  **`CREATE_SUCCESS` gönderiliyor**, DB değişmedi.
- D5 (DB açık, INSERT sunucuda reddedildi — testte 1062): `player_index` **başka hesabın karakterine** bağlandı.
- Gerçek akışta: game `CREATE_SUCCESS`'teki id'yi hesap tablosuna yazıyor (`game/input_db.cpp:220`) ve db
  `QUERY_PLAYER_LOAD` yüklenen `player_id`'nin hesaba ait olduğunu **doğrulamıyor** (`db/ClientManagerPlayer.cpp:208-260`).
  D4 sonrası o slot seçilirse başka hesabın karakteri yüklenebilir (önbellekteyse DB kapalıyken bile). **Bu yük adımı
  uçtan uca denenmedi (Unverified);** zincirin her halkası kodda ve D4'te kanıtlı.
- `player.name` benzersiz değil (`server/sql/player.sql:1031` `KEY name_idx`), yani D5'teki 1062 gerçek isimle oluşmaz;
  DB açıkken INSERT'i reddettirecek başka bir yol **bulunmadı** (Unverified). Gerçekçi tetikleyici: oluşturma anında DB
  hatası/yeniden başlatma.

## 3. Ana thread maliyeti (D6)
DB kapalıyken bir `DirectQuery`, Connector'ın yeniden bağlanma denemesi kadar sürer: medyan eski ~0,4 ms / 2a ~0,35 ms
(çağrı başına 1 bağlantı denemesi, her ikisinde 200 çağrıda 201). **Ama** FreeBSD kapalı porta RST'yi ~200/sn ile sınırlıyor
(`dmesg`: "Limiting tcp reset response … 200 packets/sec", `net.inet.icmp.icmplim=200`) ve yanıtsız SYN 1 sn sonra yeniden
gönderiliyor (`net.inet.tcp.rexmit_initial=1000`): 1000 çağrıda eski 5/4, 2a 4/4 çağrı **~1 sn** sürdü. Yani kesintide
saniyede 200'den fazla bağlantı denemesi olursa (bütün süreçler toplamı) game ana döngüsü çağrı başına ~1 sn takılabilir.
**2a'dan önce de var**; 2a'nın worker'ı ulaşılamayan bağlantı başına 100 ms'de bir dener (bağlantı başına 10/sn), eski kod
K7a'da 5 sn'de >1000 deneme yapıyordu.

## 4. Çağrı tablosu
Ortak sütunlar tekrar edilmez:
- **Thread:** game'deki bütün çağrılar game ana döngüsünde (tek thread; `config.cpp` açılışta döngüden önce); db'deki bütün
  çağrılar db ana döngüsünde (tek thread). Başka thread'den `DirectQuery` yok.
- **Eski ↔ 2a:** her satırda aynı (bölüm 1): D1'de ikisi de hemen hata; fark yalnız D2'de (eski: donar → uygular; 2a: hata).
  "Başarısızlıkta" sütunu her ikisinde de D1'de olanı yazar.
- **Kontroller:** `errno` hiçbir satırda yok. `rows` = `uiNumRows`, `aff` = `uiAffectedRows`, `ins` = `uiInsertID`, `-1` =
  `(uint32_t)-1` kontrolü. "Get NULL" kontrolü anlamsız (hiçbir zaman NULL değil); `pSQLResult` NULL kontrolü ayrıca yazıldı.

Sınıflar: **G** güvenli hata (işlem yapılmaz, kullanıcı/protokol hata görür) · **V** görünür başarısızlık (sonuç yanlış ama
fark edilir) · **S** sessiz veri kaybı/tutarsızlık · **E** ekonomi tutarsızlığı (yang/item) · **C** çökme · **T** yalnız teşhis/
etkisiz · **B** açılış (açılmazsa süreç çıkar ya da eksik açılır).

### game
| # | Yer | Fonksiyon | SQL | R/W | Alan | Kontrol | Başarısızlıkta | Sınıf | Risk |
|---|---|---|---|---|---|---|---|---|---|
| g1 | `game/config.cpp:870` | `config_init` (CommonSQL) | select.locale | R | config | rows==0 → `exit(1)` | çekirdek açılmaz | B | Düşük |
| g2 | `game/config.cpp:938` | `config_init` | select.locale (SKILL_POWER_BY_LEVEL) | R | denge | rows==0 → `exit(1)` | çekirdek açılmaz | B | Düşük |
| g3 | `game/config.cpp:979` | `config_init` (meslek başına) | select.locale | R | denge | rows==0 → varsayılan tablo | **sessizce varsayılan beceri tablosu** | B/S | Orta (açılışta, D2 penceresi yok) |
| g4 | `game/db.cpp:409` | `DBManager::AnalyzeReturnQuery` (auth, `LC_IsEurope`) | update.account (last_play) | W | hesap | yok | `last_play` güncellenmez | T | Düşük |
| g5 | `game/char_change_empire.cpp:35` | `CHARACTER::ChangeEmpire` adım 1 | select.player_index | R | hesap | rows==0 → 0 | işlem yapılmaz (0) | G | Düşük |
| g6 | `game/char_change_empire.cpp:63` | `ChangeEmpire` adım 2 (×4) | select.guild_member | R | lonca | rows>0 | **lonca kontrolü atlanır** (loncalı karakter imparatorluk değiştirebilir) | S | Orta |
| g7 | `game/char_change_empire.cpp:104` | `ChangeEmpire` adım 4 | update.player_index | W | hesap/**yang+item** | `aff > 0` | `aff=~0` → **999 döner**: quest 500.000 yang + item 71054 alır (`change_empire.quest:102-106`), DB değişmez | **E** | **Yüksek** (pencere: adım 1 başarılı, adım 4 başarısız) |
| g8 | `game/char_change_empire.cpp:128` | `GetChangeEmpireCount` | select.change_empire | R | sayaç | rows==0 → 0 | sayaç 0 sanılır | S | Düşük |
| g9 | `game/char_change_empire.cpp:169` | `SetChangeEmpireCount` | insert/update.change_empire | W | sayaç | yok | sayaç artmaz (limit aşılabilir) | S | Düşük |
| g10 | `game/char_change_empire.cpp:180` | `GetAID` | select.player_index | R | hesap | rows==0 → 0 | 0 → çağıranlar erken döner | G | Düşük |
| g11 | `game/guild.cpp:77` | `CGuild::CGuild` (kurma) | insert.guild | W | lonca/**yang** | yok (`// TODO if error occur?`) | `guild_id = ins` = **önceki INSERT'in id'si** ya da 0. ≠0 ise `CreateGuild` başarı → 200.000 yang alınır (`input_main.cpp:2520-2536`), lonca başka id ile bellekte + `GUILD_CREATE` db'ye; 0 ise yang alınmaz ama id 0'lı lonca map'e girer | **E/S** | **Yüksek** (pencere: `guild_manager.cpp:84` SELECT başarılı, INSERT başarısız) |
| g12 | `game/guild.cpp:1058, 1060` | `CGuild::DeleteComment` | delete.guild_comment | W | lonca notu | `aff==0 \|\| -1` | "silinemez" mesajı | G | Düşük |
| g13 | `game/guild.cpp:1081` | `RefreshCommentForce` | select.guild_comment | R | lonca notu | yok (rows sayısı kadar) | boş liste | V | Düşük |
| g14 | `game/guild.cpp:2125` | `CGuild::VerifyGuildJoinableCondition` (`LC_IsBrazil`) | select.guild_invite_limit | R | lonca | rows>0 | limit atlanır | — | Ölü (english locale) |
| g15 | `game/guild_manager.cpp:84` | `CGuildManager::CreateGuild` | select.guild (ad kontrolü) | R | lonca | rows==0 → hata mesajı | "kurulamaz" | G | Düşük |
| g16 | `game/guild_manager.cpp:207` | `CGuildManager::Initialize` | select.guild | R | lonca | yok | **hiç lonca yüklenmez** (açılış) | B/S | Orta (açılışta) |
| g17 | `game/questlua_building.cpp:100` | quest `building.has_land` | select.land | R | arsa | rows>0 | false (arsa yok sanılır) | V | Düşük |
| g18 | `game/questlua_pc.cpp:2119` | quest `pc.change_name` | select.player (ad var mı) | R | karakter | rows>0 | **aynı ad kontrolü atlanır** | S | Orta |
| g19 | `game/questlua_pc.cpp:2148` | quest `pc.change_name` | update.player (name) | W | karakter/**item** | yok | `SetNewName` + 4 döner → quest **item'ı siler** (`change_name.quest:82-88`), DB'de ad değişmez | **E** | **Yüksek** |

### db
| # | Yer | Fonksiyon | SQL | R/W | Alan | Kontrol | Başarısızlıkta | Sınıf | Risk |
|---|---|---|---|---|---|---|---|---|---|
| d1 | `db/ClientManager.cpp:749` | `RESULT_SAFEBOX_LOAD` (item_award → depo/mall) | insert.item | W | **item** | `aff==0 \|\| ins==0 \|\| -1` → break | ödül teslim edilmez, `taken` yazılmaz (sonra tekrar) | G | Düşük |
| d2 | `db/ClientManager.cpp:969` | `QUERY_EMPIRE_SELECT` | update.player_index | W | hesap | yok | game'e her zaman başarı; imparatorluk DB'ye yazılmaz | S | Orta |
| d3 | `db/ClientManager.cpp:976, 1021` | `QUERY_EMPIRE_SELECT` | select.player_index, update.player | R/W | karakter konumu | rows | konum güncellenmez | S | Düşük |
| d4 | `db/ClientManager.cpp:1763` | `CreateObject` (lonca binası) | insert.object | W | **yapı** | `ins==0` | `ins` = önceki INSERT'in id'si → **yanlış id'li nesne** belleğe ve oyunculara | S | Orta |
| d5 | `db/ClientManager.cpp:1797` | `DeleteObject` | delete.object | W | yapı | `aff==0 \|\| -1` | silinmez, log | G | Düşük |
| d6 | `db/ClientManager.cpp:1852` | `BlockChat` | select.player | R | GM | rows | engel uygulanmaz (GM'e geri bildirim yok) | V | Düşük |
| d7 | `db/ClientManager.cpp:2959` | `InitializeLocalization` (açılış) | select.locale | R | config | rows==0 → false | db açılmaz | B | Düşük |
| d8 | `db/ClientManager.cpp:3356, 3405` | `__GetAdminInfo`, `__GetHostInfo` (çekirdek açılışı, `ReloadAdmin`) | select.gmlist, select.gmhost | R | GM yetkisi | rows==0 → false | **GM listesi boş gider** | V | Düşük |
| d9 | `db/ClientManagerBoot.cpp:106, 444, 523, 859, 940, 971, 1045, 1123, 1226, 1295` | `Initialize*Table` (açılış + `QUERY_RELOAD_PROTO`) | select.* proto/shop/land/object | R | proto | çoğu rows==0 → false; refine/banword → true; land/object önce temizler | açılışta db açılmaz; **yeniden yüklemede** land/object tabloları boşalabilir | B/S | Düşük–Orta (sadece GM reload) |
| d10 | `db/ClientManagerEventFlag.cpp:13` | `LoadEventFlag` (açılış) | select.quest | R | etkinlik | rows | bayraklar yüklenmez | B/S | Düşük |
| d11 | `db/ClientManagerGuild.cpp:36, 41` | `GuildAddMember` | insert/select.guild_member | W/R | lonca | rows==0 → return | üye eklenmez, game'e paket gitmez | V | Düşük |
| d12 | `db/ClientManagerHorseName.cpp:11` | `UpdateHorseName` | replace.horse_name | W | kozmetik | yok | ad yazılmaz, diğer çekirdeklere yine iletilir | S | Düşük |
| d13 | `db/ClientManagerHorseName.cpp:22` | `AckHorseName` | select.horse_name | R | kozmetik | rows | boş ad | V | Düşük |
| d14 | `db/ClientManagerLogin.cpp:151` | `RESULT_LOGIN_BY_KEY` (player_index yok) | select.player_index | R | hesap | rows==0 → INSERT (ReturnQuery) | gerçekten var olan index'e INSERT dener → kalıcı hata | V | Düşük |
| d15 | `db/ClientManagerLogin.cpp:477` | `QUERY_CHANGE_NAME` | select.player | R | karakter | rows, `pSQLResult` | `PLAYER_CREATE_FAILED` | G | Düşük |
| d16 | `db/ClientManagerLogin.cpp:504` | `QUERY_CHANGE_NAME` | update.player | W | karakter | yok | game'e **başarı**; DB'de ad değişmez (bir sonraki girişte yine sorulur) | S | Orta |
| d17 | `db/ClientManagerPlayer.cpp:834, 868` | `__QUERY_PLAYER_CREATE` | select.player_index, select.player | R | hesap | rows, `pSQLResult` | `PLAYER_CREATE_FAILED` | G | Düşük |
| d18 | `db/ClientManagerPlayer.cpp:926` | `__QUERY_PLAYER_CREATE` | insert.player | W | **hesap/karakter** | `aff <= 0` (uint32) | `aff=~0` geçer, `player_id = ins` = **önceki karakterin id'si** (D4/D5) | **S (sahiplik)** | **Kritik** |
| d19 | `db/ClientManagerPlayer.cpp:941, 948` | `__QUERY_PLAYER_CREATE` | update.player_index, delete.player | W | hesap | `aff <= 0` | `aff=~0` geçer → **`CREATE_SUCCESS` stale id ile** (D4); DB açıksa `player_index` başka karaktere bağlanır (D5) | **S (sahiplik)** | **Kritik** |
| d20 | `db/ClientManagerPlayer.cpp:1088, 1140` | `__RESULT_PLAYER_DELETE` | insert.player_deleted, update.player_index | W | karakter | `aff==0 \|\| -1` | `DELETE_FAILED` | G | Düşük |
| d21 | `db/ClientManagerPlayer.cpp:1151, 1154` | `__RESULT_PLAYER_DELETE` | delete.player, delete.item | W | karakter/item | yok | index'ten ayrılmış ama satırı/item'ları kalan karakter (pencere: d20 başarılı, bunlar başarısız) | S | Orta |
| d22 | `db/DBManager.cpp:122` | `CDBManager::DirectQuery` (sarmalayıcı) | — | — | — | — | — | — | — |
| d23 | `db/GuildManager.cpp:162, 193` | `CGuildManager::Initialize`, `Load` | select.guild | R | lonca | rows | lonca db belleğine yüklenmez (`Load` ← `GuildCreate`: game'de kurulan lonca db'nin listesinde yok) | S | Orta (`Load`, g11 ile aynı pencere) |
| d24 | `db/GuildManager.cpp:904` | `BootReserveWar` (açılış) | select.guild_war_reservation | R | lonca savaşı | rows==0 → continue | rezervasyonlar yüklenmez | B/S | Düşük |
| d25 | `db/GuildManager.cpp:965, 980` | `GetAverageGuildMemberLevel`, `GetGuildMemberCount` (← `ReserveWar`) | select.guild_member | R | lonca savaşı | **yok**: `mysql_fetch_row(NULL)` → `row[0]` | **db süreci çöker** (NULL okuma) | **C** | **Yüksek** |
| d26 | `db/GuildManager.cpp:1068` | `ReserveWar` | insert.guild_war_reservation | W | lonca savaşı | `aff==0 \|\| ins==0 \|\| -1` | false | G | Düşük |
| d27 | `db/GuildManager.cpp:1190, 1193, 1196` | `ChangeMaster` | update.guild, update.guild_member ×2 | W | lonca | yok, her zaman true | game'e **ACK**; DB'de lider/yetki değişmez ya da yarım değişir | S | Orta |
| d28 | `db/GuildManager.cpp:1217` | `CGuildWarReserve::Initialize` | select.guild_war_bet | R | **yang (bahis)** | rows | bahisler yüklenmez → savaş sonunda ödeme/iade yapılmaz | E | Orta (açılış/rezervasyon yükleme) |
| d29 | `db/GuildManager.cpp:1294` | `CGuildWarReserve::Bet` | insert.guild_war_bet | W | **yang** | `aff==0 \|\| -1` → false | yang quest'te **önceden düşüldü** (`guild_war_bet.quest:65-66`), iade yok | **E** | Orta |
| d30 | `db/HB.cpp:22` | `PlayerHB::Initialize` | show.create table | R | yedek | rows==0 → false | — | T | Düşük |
| d31 | `db/HB.cpp:78` | `PlayerHB::Query` | create.table | W | yedek | — | çağıranı yok (ölü) | — | Ölü |
| d32 | `db/ItemIDRangeManager.cpp:95, 125` | `BuildRange` (açılış, `Peer` yıkıcısı) | select.item (MAX, COUNT) | R | **item id** | ikinci sorgu koruma: COUNT da başarısızsa false | aralık kullanılmaz | G | Düşük |
| d33 | `db/Marriage.cpp:43, 45` | `CManager::Initialize` (açılış) | delete/select.marriage | W/R | evlilik | rows | evlilikler yüklenmez | B/S | Düşük |
| d34 | `db/Marriage.cpp:104, 144, 190, 234` | `Add`, `Update`, `Remove`, `EngageToMarriage` | insert/update/delete.marriage | W | evlilik | `aff==0 \|\| -1` → return | işlem yapılmaz, log | G | Düşük (game tarafındaki ücret kontrol edilmedi) |

Toplam: game 20 çağrı noktası (19 satır), db 57 çağrı noktası + sarmalayıcı (34 satır); sarmalayıcılar `game/db.cpp:68`
(`DBManager::DirectQuery`), `game/db.cpp:599` (`AccountDB::DirectQuery`), `db/DBManager.cpp:122` (`CDBManager::DirectQuery`).
Liste: `grep -rn "DirectQuery\s*(" server-src/src/{game,db}` (tanımlar ve yorumlar hariç).

## 5. Sonuç ve öneri
1. **2a'nın kendi farkı (D2 penceresi):** eski davranış güvenli bir alternatif değildi: kesinti sürdükçe ana döngüyü sınırsız
   donduruyordu. Bekleme/tekrar geri getirmek (ya da keyfi süre/sayı) önerilmiyor.
2. **Asıl risk 2a'dan önce de var ve kesintinin genel durumunda (D1) bugün de oluyor:** g7, g11, g19 (ekonomi), d18/d19
   (karakter sahipliği), d25 (çökme). 2a bunları ne yaratıyor ne düzeltiyor; D2 penceresini de onlara ekliyor.
3. **Kök neden düzeltmesi önerisi (onay gerekir, 2a'ya ayrı commit adayı):** başarısız bir ifadenin sonucu başarılı bir
   sonuç gibi görünmesin: `libsql`'de başarısızlıkta `uiAffectedRows = 0`, `uiInsertID = 0`. Bu tek değişiklik
   kanıtlanmış yollarda sonucu güvenli hataya çevirir: d18/d19 (`<= 0` → `PLAYER_CREATE_ALREADY`/`FAILED`), g7 (`> 0` → 0,
   yang alınmaz), d4 (`ins==0` → nesne kurulmaz), g11 (`guild_id=0` → yang alınmaz; id 0'lı lonca nesnesi kalır, ayrı
   düzeltme). `(uint32_t)-1` kontrolü yapan çağıranlar etkilenmez (`== 0` da kontrol ediyorlar).
   **Çözmediği:** g19 ve d16/d27/d2 (sonuç hiç okunmuyor), d25 (NULL okuma), g6/g18 (SELECT hatası "satır yok" sayılıyor),
   d29 (yang önceden düşülüyor). Bunlar çağıran düzeltmesi ister → ayrı iş, öncelik sırası: d25, g19, d29, d16/d27, g6/g18.
4. **Ana thread maliyeti (bölüm 3):** RST hız sınırı yüzünden kesintide ~1 sn'lik takılmalar 2a'dan önce de var; ayrı teşhis/
   tasarım maddesi (sayı icat edilmeden: önce production benzeri yükte ölçüm).

## 6. A-17 düzeltmesi (2a'ya alındı) ve sonrası
**Kapsam:** (1) `libsql`: başarısız ifadenin sonucu Connector'dan hiçbir şey okumaz (`SQLMsg::StoreFailed`: sonuç kümesi
yok, `uiNumRows = uiAffectedRows = uiInsertID = 0`; `Get()` yine NULL değil). Başarılı ifadede `SQLMsg::StoreApplied`,
denemenin okuduğu sonuç kümesini parametre olarak alır (mesaja alan eklenmedi: üretici maliyeti, worklog). (2) Red-team'de bulunan ikinci boşluk:
sonuç kümesinin **satırları** `mysql_store_result` ile okunurken bağlantı koparsa ya da sorgu öldürülürse ifade eskiden
"uygulandı, 0 satır" görünüyordu (D8: `errno=0`); satırlar artık denemenin parçası (`CAsyncSQL::Attempt`), okunamazsa
deneme okuma evresinde başarısız (D8: `errno=1317`, sonuç `ambiguous`, tekrar yok). (3) `db/GuildManager.cpp`
`GetAverageGuildMemberLevel` / `GetGuildMemberCount`: sonuç yoksa `false` (`libsql/SQLRead.h` `SQLReadFirstInt`); `ReserveWar`
bu iki okumayı en başa aldı (altın alınmadan önce), okunamazsa rezervasyonu reddeder. Başka sıra ya da değer değişmedi.

**Kanıt (`sqlprobe`, eski `8fbec589` ↔ 2a + A-17, her biri 3/3):**
| Senaryo | Önce | Sonra |
|---|---|---|
| D7: başarılı INSERT (tek/çok satır), UPDATE (3 satır / değişmeyen), REPLACE, DELETE, SELECT (satırlı/boş/COUNT), INSERT…SELECT, INSERT IGNORE; DirectQuery ve worker yolu | — | **birebir aynı** (22 satır: errno, sonuç kümesi, satır, affected, insert id, ilk satır) |
| D3: başarılı INSERT'ten sonra başarısız INSERT | `affected=4294967295 insert_id=1` | `affected=0 insert_id=0` |
| D3: başarısız UPDATE / SELECT | `affected=4294967295` | `affected=0`; `Get()` NULL değil, sonuç kümesi yok |
| D4: karakter oluşturma kontrolleri, DB kapalı | `CREATE_SUCCESS`, eski karakterin id'si | `create failed`, id 0, `player_index` değişmedi |
| D5: aynı, INSERT reddedildi (DB açık) | `player_index` başka karaktere bağlandı | `create failed`, `player_index` değişmedi |
| D8: satırlar gelirken sorgu öldürüldü | `errno=0`, 0 satır (sessiz) | `errno=1317`, başarısız |
| G3: GuildManager sorguları, başarı | ortalama 15, sayı 2; üyesiz lonca 0/0 | aynı |
| G3: GuildManager sorguları, DB kapalı | **SIGSEGV** (eski kod, ayrı süreçte) | `refused` |

**Uçtan uca karakter oluşturma → yükleme denenmedi:** bunun için bir istemcinin (ya da protokol öykünücüsünün) gerçek
db + game'e karakter oluşturma isteği göndermesi ve DB'nin tam o anda hata vermesi gerekir; headless istemci yok (roadmap 2.2)
ve VM'de kurulum/yeniden başlatma bu kapıda yasak. Yerine: D4/D5 gerçek kontrollerin birebir kopyasıyla aynı `libsql` üzerinde
koşuldu; düzeltmeden sonra `CREATE_SUCCESS` dalına hiç girilmiyor, dolayısıyla yükleme zincirinin ilk halkası kopuyor.
`QUERY_PLAYER_LOAD`'ın sahiplik kontrolü olmaması ayrı bir derinlemesine savunma eksiği olarak A-17 çağıran işinde kalır.

**Denetim satırlarının yeni durumu:**
| Satır | Yeni durum | Dayanak |
|---|---|---|
| d18/d19 karakter oluşturma | **güvenli hata** (`PLAYER_CREATE_ALREADY` / `FAILED`) | D4/D5 |
| g7 imparatorluk değiştirme (`aff > 0`) | **güvenli hata** (0 döner, quest yang/item almaz) | D3 (başarısız UPDATE `affected=0`) + kod |
| d4 lonca binası (`ins == 0`) | **güvenli hata** (nesne kurulmaz) | D3 (başarısız INSERT `insert_id=0`) + kod |
| d25 `ReserveWar` | **güvenli hata**, çökme yok, altın alınmaz | G3 + kod |
| g11 lonca kurma | **güvenli hata** (`fix/a17-g11-guild-create`): INSERT `APPLIED` ve `insert_id != 0` değilse kurucu hiçbir yan etki başlatmaz (derece satırları, `GUILD_CREATE`, ad yayını, üye ekleme, işaret yuvası), `CreateGuild` 0 döner, ücret alınmaz. Düzeltme öncesi test VM'de: hayalet lonca 0 + `guild_member (pid, 0)` + 15 `guild_grade (0, *)` + `mark_index` `0 1`; yeniden başlatmadan sonra yeni kurulumda 200.000 yang alındı, lider eklenmedi (`UNIQUE pid`) | test VM önce/sonra + `tools/guild/test-guild-create-logic.cpp` |
| d11 lonca üye ekleme | **düzeltilmedi.** Yalnız savunma katmanı: `guild_id = 0` reddediliyor (`fix/a17-g11-guild-create`). INSERT sonucu hâlâ okunmuyor; ardından gelen SELECT yetkili (başarısız SELECT 2a'da 0 satır → paket gitmez). Açık: sıfır olmayan ama `guild`'de olmayan id savunması yok (kanıtlanmış problem değil, hardening adayı); normal kurulumda üyelik INSERT'i başarısız olursa ücret alınmış üyesiz lonca → A-27 | kod |
| d1, d5, d20, d26, d34, g12, d29'un INSERT kontrolü | zaten güvenliydi (`== 0` da kontrol ediliyordu), değişmedi (d29'un iadesi için aşağıdaki satır) | kod |
| g19 isim değiştirme | **güvenli hata** (`fix/a17-change-name`): UPDATE `APPLIED` + 1 satır değilse dönüş 6; item, bekleme süresi, messenger ve log korunur; AMBIGUOUS de 6 (oyuncu lehine) | test VM uçtan uca (önce: item kaybı) + `tools/change-name/test-change-name-logic.cpp` |
| d2, d16, d27, d12, d21, g4, g9 | **değişmedi:** sonuç hiç okunmuyor | → A-17 çağıran işi |
| g18 ad kontrolü (SELECT hatası) | **güvenli hata** (`fix/a17-change-name`): okunamayan sayım dönüş 6, "ad boş" sayılmıyor. **Kalan:** aynı anda aynı adı seçen iki oyuncu (`player.name` `UNIQUE` değil) | aynı |
| g6, g3, g16, d9, d23, d24, d28, d33 | **değişmedi:** hata ile boş sonuç ayrılmıyor | → A-17 çağıran işi |
| d29 lonca savaşı bahsi | **audit düzeltmesi (2026-10-11, kod kanıtı):** "telafi yok" **yanlıştı**. Çağıran `CGuildManager::Bet` (`db/GuildManager.cpp:1162-1187`) her ret yolunda (rezervasyon yok, geçersiz lonca, savaş başlamış, zaten bahis, INSERT hatası) yangı `item_award` (50026 Coins, `socket0`) olarak iade ediyor; iade depoyu açınca teslim ediliyor. A-17 kod değişikliği gerekmiyor. **Runtime kabulü bekliyor:** İngilizce bahis quest'i `_80_say` biçim hatasıyla çöktüğü için akışa ulaşılamıyordu; T1-T4 + beraberlik (T6) `fix/guild-war-bet-quest` kabulünde. Açık: AMBIGUOUS/yeniden başlatma çift değeri, önbellek kaybı, `taken_time` teslim yarışı → A-27 | kod; runtime bekliyor |

**"Bir SQL hatası çağırana başarı metadata'sı olarak görünebilir mi?" (red-team):**
- DirectQuery, ReturnQuery, kapanışta çalıştırılmayan: başarısızlıkta `uiSQLErrno != 0`, sonuç sıfır. **Hayır.**
- Sonuç kümesinin satırları okunamazsa: artık başarısız (D8). **Hayır.**
- Başarılı bir SELECT'te `uiInsertID` bağlantıdaki son INSERT'in id'sidir (D7 `sel_count insert_id=5`, eskiyle aynı;
  Connector bunu sadece OK paketinde günceller). Bu bir hata değil ama anlamsız bir alan; INSERT dışında `uiInsertID` okuyan
  çağıran yok (`grep uiInsertID`: hepsi INSERT sonrası).
- İkinci sonuç kümesi (yalnız stored procedure; `CALL` yok, çoklu ifade kapalı) hata verirse `StoreApplied` döngüsü bunu
  görmez. Bugün erişilemez; `CALL` eklenirse yeniden değerlendirilmeli.
- Sonucu hiç okumayan ya da SELECT hatasını "satır yok" sayan çağıranlar (yukarıdaki tablo): metadata doğru ama
  okunmuyor → A-17 çağıran işi.
