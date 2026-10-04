# Oyun Bilgileri

Son doğrulama: 2026-10-04 @ 5452c50f

Bu files'ın oynanış bilgileri, her biri onu belirleyen dosyayla. Bir değer değişirse aynı PR'da
burada da güncelle. Kanıtlanmayan değerler "Doğrulanmayanlar" bölümüne gider.

Bir sistemin anlatımı ~30 satırı geçerse `docs/systems/<sistem>.md`'ye taşı ve burada tek satırlık
link bırak.

## Temel değerler

| Bilgi | Değer | Kanıt |
|---|---|---|
| Maksimum level | **120** | `server/share/conf/game.txt:18` (`MAX_LEVEL: 120`); kod sınırı `PLAYER_MAX_LEVEL_CONST = 120` (`server-src/src/common/length.h:39`, `game/config.cpp:717`); ayar yoksa varsayılan 99 (`config.cpp:110`) |
| Exp tablosu | 120 seviyeye kadar | `PLAYER_EXP_TABLE_MAX = 120` (`length.h:38`) |
| İmparatorluklar | 1 Shinsoo · 2 Chunjo · 3 Jinno | `server-src/src/game/start_position.cpp:5-11` (Korece isimler) |
| Başlangıç haritası | Shinsoo 1 · Chunjo 21 · Jinno 41 | `start_position.cpp:17-23` |
| Kanallar | Tasarım CH1–CH4 + CH99; test VM'inde CH1 + CH99 | `server/channels.py:14-20`; VM `/etc/rc.conf` `m2dev_channels="1"` |
| Oranlar (exp / drop / yang) | Global oran ayarı yok | `game/config.cpp`'de oran token'ı bulunamadı. Oranlar muhtemelen imparatorluk bonusu ya da etkinlik bayraklarıyla (Unverified) |
| Client dilleri | ae br cz de dk en es fr gr hu it nl pl pt ro ru tr (+ common) | `client/assets/locale/locale/` |
| Server quest dili | english | `server/share/locale/english/` |

## Haritalar

Tam liste: `server/share/locale/english/map/index` (66 harita). Çekirdek dağılımı: `docs/architecture.md`.

| Map index | Ad | Yükleyen çekirdek |
|---|---|---|
| 1 | metin2_map_a1 (Shinsoo köyü) | core1 |
| 21 | metin2_map_b1 (Chunjo köyü) | core2 |
| 41 | metin2_map_c1 (Jinno köyü) | core3 |
| 66 | metin2_map_deviltower1 | core3 |
| 72 / 73 | metin2_map_skipia_dungeon_01 / 02 | core1 / core2 |
| 104 | metin2_map_spiderdungeon | core3 |
| 208 | metin2_map_skipia_dungeon_boss | core1 |
| 351 | metin2_map_n_flame_dungeon_01 | core3 |
| 352 | metin2_map_n_snow_dungeon_01 | core2 |

## Aktif sistemler

Server `game` build'i bütün `.cpp`'leri derler (`server-src/src/game/CMakeLists.txt:1`, `GLOB_RECURSE`), bu yüzden
aktifliği flag'ler ve quest listesi belirler.

| Sistem | Aktiflik kanıtı | Not |
|---|---|---|
| Kostüm | client `ENABLE_COSTUME_SYSTEM` (`client-src/src/UserInterface/Locale_inc.h:1`) | |
| Enerji | client `ENABLE_ENERGY_SYSTEM` (`Locale_inc.h:2`) | |
| Simya (Dragon Soul) | client `ENABLE_DRAGON_SOUL_SYSTEM` (`Locale_inc.h:3`); `dragon_soul*.quest` 5 quest `locale_list`'te | |
| Yeni ekipman | client `ENABLE_NEW_EQUIPMENT_SYSTEM` (`Locale_inc.h:4`) | |
| Pet | server `__PET_SYSTEM__` (`server-src/src/common/service.h:6`) | |

Kapalı: `ENABLE_DISCORD_RPC` (`Locale_inc.h:6`, yorum satırı).

## Zindanlar

Aktif = quest'i `server/share/locale/english/quest/locale_list`'te.

| Zindan | Quest | Harita |
|---|---|---|
| Şeytan kulesi | `deviltower_zone.quest` | 66 |
| Şeytan katakombu | `devilcatacomb_zone.quest` | Unverified |
| Örümcek zindanı | `spider_dungeon_2floor.quest`, `spider_dungeon_3floor_boss.quest` | 104 + Unverified |
| Ejderha yuvası | `dragon_lair.quest`, `dragon_lair_access.quest`, `dragon_lair_weekly.quest` | 208 (`dragon_lair_access.quest:107`) |
| Alev zindanı | `flame_dungeon.quest`, `event_flame_dungeon_open.quest` | 351 |
| Kar zindanı | `snow_dungeon.quest` | 352 |

Dosyası var ama **aktif değil** (`locale_list`'te yok): `deviltower_2.quest`, `xxx_monkey_dungeon.quest`.

## Doğrulanmayanlar

- Boss mob vnum'ları ve drop'ları: quest ve `mob_proto.txt`'ten çıkarılmadı.
- Exp/drop/yang oranlarının nasıl ayarlandığı: global bir config bulunamadı; imparatorluk bonusu / etkinlik bayrakları incelenmedi.
- Kostüm, enerji, simya ve yeni ekipmanın server tarafında ayrıca bir flag'i var mı: server'da bu isimlerde flag yok; kodun koşulsuz derlendiği varsayılıyor (doğrulanmadı).
- `devilcatacomb_zone` ve örümcek zindanı 3. katın harita index'leri.
