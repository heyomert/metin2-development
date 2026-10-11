# Lonca savaşı bahsi: d29 "telafi yok" yanlıştı, İngilizce bahis quest'i her denemede çöküyordu

- **Tarih:** 2026-10-11
- **Tür:** düzeltme / karar
- **Alan:** server (quest, dil dosyaları) / db (keşif)
- **Durum:** Aktif
- **PR / commit:** `fix/guild-war-bet-quest` (PR açılınca link)

## Problem / hedef
A-17 audit'i d29 için "yang quest'te önceden düşülüyor, iade yok" diyordu. Doğrulamak için bahis akışı uçtan uca
yeniden kuruldu ve test VM'de denendi. Bahse hiç ulaşılamadı: İngilizce bahis quest'i, lonca seçimi ekranından önce Lua
hatasıyla duruyordu.

## Kök neden / kanıt
**Bahis akışı ve iade (kod):**
- Quest önce `pc.changegold(-tutar)`, sonra `guild.war_bet(...)` (`guild_war_bet.quest:65-66`); game `GD::GUILD_WAR_BET`
  gönderir, cevap beklemez (`game/questlua_guild.cpp:177-199`).
- db `CGuildManager::Bet` (`db/GuildManager.cpp:1162-1187`): rezervasyon yoksa ya da `CGuildWarReserve::Bet` false dönerse
  **`INSERT INTO item_award (login, 50026, socket0=yang)`** (asenkron). `CGuildWarReserve::Bet` (`:1283-1340`) şunlarda false:
  lonca taraf değil, savaş başlamış, hesabın bahsi var (`mapBet`), `INSERT guild_war_bet` `uiAffectedRows == 0 || -1`
  (2a'dan sonra başarısız sorguda 0). Başarıda toplamlar, `mapBet` ve bütün çekirdeklere `DG::GUILD_WAR_BET`.
- İade teslimi: db `item_award`'ı 5 sn'de bir okur (`db/ClientManager.cpp:2771-2773`); `mall=0` olduğu için depo açılınca
  depoya item yazılır, sonuç kontrol edilir, `taken_time` yazılır (`:620-772`). `count` 0 yazılıyor ama 50026 `ITEM_USE` ve
  yığınlanamaz, `CreateItem` 1 yapar (`game/item_manager.cpp:261-271`); kullanınca `socket0` kadar yang
  (`game/char_item.cpp:3770-3776`).
- Açılışta devam eden ya da zamanı geçmiş rezervasyon berabere sayılır, bahisler iade edilir (`BootReserveWar` → `Draw`).
- **Sonuç:** "iade yok" yanlış; A-17 tipi kod değişikliği gerekmiyor. Açık kalanlar A-27 sınıfında (AMBIGUOUS + yeniden
  başlatma çift değeri, önbellek kaybı, `taken_time` teslim yarışı) ve A-34 (deposu `INACTIVE` hesapta iade erişimi).

**Quest çökmesi (runtime, test VM, eski quest):**
- `LUA_ERROR: [string "guild_war_bet"]:28: bad argument #3 to 'format' (string expected, got no value)` (03:14:42).
  `string.format(_80_say, name1.." "..name2.._45_say..g[s][4])` tek değer veriyor; İngilizce `_80_say` üç bekliyor
  (`%s %s %d`). Quest yang düşmeden duruyor, kimse bahis yapamıyordu.
- Aynı quest'te: `select(name1, name1)` (ikinci düğme yanlış ad), liste satırında `_35_table` şablonu `..` ile birleştiriliyor
  ("PrGuild%s against %s BetBeta"), ret durumunda da "Everything is ready…" başarı mesajı.

**Dil dosyaları:**
- game yalnız `locale/english/translate.lua`'yı yükler (`game/questlua.cpp:555-566`); `translate_<dil>.lua` otomatik geri
  düşme değil, eksik anahtar `nil` → Lua hatası.
- Yer tutucular: `_80_say` fr'de `%s %d %s`, de'de tek `%s`; `_35_table` de'de `"gegen  "`, fr'de `"gegen "` (`%s` yok).
- Anahtar paritesi: `translate.lua` 8.929, diğer 14 dosya 8.925 (A-19'un 4 `warehouse` anahtarı yalnız İngilizcede).

## Reddedilen yaklaşımlar
- **Geçici, yalnız VM'de quest yaması ile test:** test edilen şey repodaki exact commit olmalı.
- **`_115_say`'i yalnız en+tr'ye eklemek:** yeni parite borcu ve başka dil `translate.lua` yapılınca çökme. Bütün dillere
  eklendi; en ve tr çeviri, diğer 13 dilde İngilizce yedek (A-36 çeviri kalitesi borcu).
- **db→game sonuç paketiyle kesin kabul/ret mesajı:** protokol değişikliği; bu PR'da dürüst ortak mesaj (A-33).
- **Fiyat düğmesindeki `locale.gold` düzeltmesi:** anahtar diğer quest'lerde sonek olarak kullanılıyor ve de/fr'de şablon
  değil; tek quest'te değiştirmek tutarsızlık yaratır → A-36.

## Çözüm
- `guild_war_bet.quest`: `_35_table` ve `_80_say` `string.format(anahtar, name1, name2[, points])`; `select(name1, name2)`;
  bahisten sonra `_115_say`.
- 15 `translate*.lua`: `_115_say` hepsinde; de `_35_table` `"%s gegen %s "`, fr `"%s contre %s "`; de ve fr `_80_say`
  mevcut cümle korunarak `name1, name2, points` sırasına. Kontrol: 15 dosyada `_35_table` `%s%s`, `_80_say` `%s%s%d`,
  `_115_say` yer tutucusuz; CRLF/UTF-8 korundu; gerçek argümanlarla biçimlendirme denemesi hatasız; `qc` derlemesi 9 nesne.
- Düzeltme bahis yang akışını ve kazanan ödemesini canlandırıyor; kazanan ödemesi uçtan uca kanıtlanmadan bahis sistemi
  production-ready sayılmaz (A-31, A-27).

## Doğrulama
- Runtime kabulü (RB-17 T1–T4 + T6, exact commit): bekliyor.

## Bir dahaki sefere tuzaklar
- Bahis listesi yalnız `type == GUILD_WAR_TYPE_BATTLE` (1) rezervasyonları gösterir; lonca sırası güce göre değişir.
- Köşeli parantezli lonca adı (`[GM-TEAM]`) quest seçim menüsünü bozar (A-37); fixture lonca adları harf+rakam olmalı.
- Fransızca metinlerde `?`/`:` öncesi bölünmez boşluk (U+00A0) var; tam metin eşleştirmede dikkat.
- Quest metni değişikliği C++ derlemesi istemez ama `translate.lua` açılışta yüklenir: kurulum = dosyalar + `qc` + yeniden
  başlatma.
