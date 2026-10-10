# Lonca kurma INSERT'i başarısız olunca hayalet lonca ve kalıcı üyelik kilidi (A-17 g11)

- **Tarih:** 2026-10-11
- **Tür:** düzeltme / karar
- **Alan:** server-src / db
- **Durum:** Aktif
- **PR / commit:** `fix/a17-g11-guild-create` (PR açılınca link)

## Problem / hedef
Lonca kurma `INSERT INTO guild` başarısız olunca (DB ayakta, SELECT geçmiş) oyuncu "Creation of the guild has failed."
görüyordu, yang alınmıyordu; ama aynı anda id 0'lı hayalet bir loncanın lideri oluyordu. Sunucu yeniden başlayınca
hayalet lonca kayboluyor, DB'deki yetim üyelik satırı ise kalıyordu: oyuncu bir sonraki normal kurulumda 200.000 yang
ödüyor, kendi loncasına eklenmiyordu.

## Kök neden / kanıt
- `game/guild.cpp` `CGuild::CGuild` (düzeltme öncesi 77-129) INSERT sonucunu okumadan `guild_id = uiInsertID` (başarısızlıkta
  2a'dan beri 0) ile şunları yapıyordu: 15 asenkron `guild_grade (0, *)` INSERT'i, `GD::GUILD_CREATE(0)` (db bütün çekirdeklere
  `GUILD_LOAD(0)`), `GD::GUILD_SKILL_UPDATE(0)`, ad yayını, `RequestAddMember` → `GD::GUILD_ADD_MEMBER(guild 0)`, `AllocMark(0)` +
  ortak `share/mark/mark_index` dosyasına yazma. `CreateGuild` 0 döndürünce `AnswerMakeGuild` yalnız ücreti almıyordu.
- db `GuildAddMember` (`db/ClientManagerGuild.cpp:25-66`) `guild_id = 0`'ı sorgusuz `guild_member`'a yazıp bütün çekirdeklere
  iletiyordu → game `AddMember` → `LoginMember` → `SetGuild(lonca 0)`.
- `guild_member.pid` `UNIQUE` (`server/sql/player.sql`): tek yetim satır oyuncunun bütün lonca üyeliklerini kilitliyor.
- Lonca açılışta `SELECT id FROM guild` ile yükleniyor (`game/guild_manager.cpp:207`); id 0 tabloda olmadığı için yeniden
  başlatmadan sonra yüklenmiyor, satırlar kalıyor.
- **Test VM, düzeltme öncesi (binary `174ef0566` = `main` `be7907d0e` kodu, hesap `g11test`/G11Char pid 13, geçici
  `BEFORE INSERT ON guild` trigger'ı yalnız `G11Test` adını reddetti; kanıt `/root/acceptance/g11-repro/`):**
  1. `G11Test`: client hata mesajı, yang 300.000; lonca panelinde G11Test, lider G11Char, 1/32, 15 derece, yetenekler.
     Defter `insert.guild … errno=1644 result=permanent`; DB `guild_member (13, 0, 1)`, 15 `guild_grade (0, *)`;
     `mark_index` += `0 1`; core3 `Guild created: guildID=0`, `AddMember PID 13`; db `GuildCreate 0`, `GuildAddMember 0 13`;
     core1/core2 `GUILD_LOAD(0)` → `LoadGuildData` "Query failed" (ham SQL syserr, A-16). `guild` tablosu değişmedi.
  2. Tam relog: hayalet lonca duruyor; NPC'de "Found guild" isim kutusu açılmadan kapanıyor (`pc.hasguild()`).
  3. Servis yeniden başlatma: lonca 0 yüklenmedi; yetim satırlar ve `mark_index` `0 1` kaldı.
  4. Normal `G11Real`: "guild has been created", yang 300.000 → 100.000 (önbellek yazıldıktan sonra DB'de de); db defter
     `insert.guild_member … errno=1062`, syserr "Query failed when getting guild member data"; panelde "üye değilsin".
- Hedefli temizlik: 1 üyelik + 30 derece satırı + G11Real silindi, `mark_index` kopyadan geri kondu. `mark_0.tga`'nın test
  öncesi kopyası yoktu (yedek yalnız SQL); yuva 1-2'de varsayılan işaret pikselleri kaldı, index'te bağlı değil.

## Reddedilen yaklaşımlar
- **Lonca kurmayı fabrika fonksiyonuna taşımak:** kurucuda INSERT'ten hemen sonra erken dönüş aynı sonucu daha küçük
  değişiklikle veriyor.
- **db `GuildAddMember`'da INSERT sonucuyla paketi durdurmak:** ardından gelen SELECT zaten yetkili (başarısız SELECT 2a'da 0
  satır → paket gitmez); aynı loncaya tekrar eklemede (kopya) SELECT'in yeniden senkron etkisini bozardı. Yalnız
  `guild_id = 0` reddi eklendi.
- **db'de lonca var mı SELECT'i (sıfır olmayan, `guild`'de olmayan id):** bu PR'da yok. Kanıtlanmış bir g11 problemi değil
  (düzeltmeden sonra game yalnız kurulmuş loncaların id'sini gönderiyor); çözülmüş sayılmaz, ayrı hardening adayı.

## Çözüm
- `game/guild_create_result.h`: `InsertCreatedGuild(bApplied, uiInsertID)` = `APPLIED && id != 0`.
- `CGuild::CGuild`: INSERT'ten hemen sonra kontrol; başarısızsa syserr `GUILD_CREATE: failed master_pid=… result=… errno=…`
  (ad yok), `guild_id = 0`, hiçbir yan etki başlamadan dönüş. `CreateGuild`: id 0'lı nesneyi siler, map'e eklemez, 0 döner →
  mevcut "Creation of the guild has failed." mesajı, ücret yok. Quest değişmedi.
- db `GuildCreate` ve `GuildAddMember`: **yalnız** `guild_id = 0` reddedilir (syserr, pid ile). d11 (üye INSERT sonucu, genel
  üyelik atomikliği) düzeltilmedi; A-27'de açık.
- **Kalan riskler:** AMBIGUOUS (INSERT uygulandı, id okunamadı) → yan etki yok ama üyesiz bir `guild` satırı kalabilir;
  normal kurulumda üyelik INSERT'i başarısız olursa ücret alınmış üyesiz lonca (A-27); `guild.name` `UNIQUE` değil (A-28 ile
  aynı sınıf); GM `/makeguild` sonuçtan bağımsız "oluşturuldu" yazıyor (yalnız GM, ücret yok).

## Doğrulama
- `tools/guild/test-guild-create-logic.cpp`: 6/6 (VM, FreeBSD clang); 3 mutant 1-3 FAIL ile yakalandı.
- game + db temiz derleme; değişen dosyalarda uyarı yok.
- **Test VM, düzeltme sonrası (binary `src=injected` `be7907d0e` `dirty=1` = çalışma ağacı; kaynak dosya özetleri
  `/root/acceptance/g11-after/tested-source.sha256`; yedek `m2dev-20261010T231033Z-consistent`; işaret dosyaları
  `g11-after/mark.before/`):**
  - **R1** trigger açık `G11Test`: hata mesajı, yang 300.000, panel "üye değilsin", NPC isim kutusu tekrar açılıyor; tablolar
    ve `mark_index`/`mark_0.tga` hash'i değişmedi; `Guild created`/`AllocMark`/`GuildCreate`/`GuildAddMember` yok; syserr
    `GUILD_CREATE: failed master_pid=13 result=permanent errno=1644`.
  - **R2** trigger kaldırıldı, aynı oturumda `G11Real`: "guild has been created", yang 100.000; `guild` id 4, 15 derece,
    `guild_member (13, 4, 1)`, `mark_index` `4 1`; panelde lider G11Char.
  - **R3** tam relog ve servis yeniden başlatma: lonca 4 yüklendi, üyelik ve 100.000 yang DB'de ve client'ta kalıcı.
  - RB-14: syserr'de ad/IP/SQL/tırnak 0, defter dosyaları `0600`; 6/6 süreç.
  - **Kanıtlamadığı:** AMBIGUOUS uçtan uca; db `guild_id = 0` reddi normal client ile tetiklenemiyor (kod incelemesi);
    üyelik INSERT'inin normal kurulumda başarısız olması (A-27).

## Bir dahaki sefere tuzaklar
- `m2dev-backup` yalnız SQL arşivliyor; `share/mark/` (işaret dosyaları) yedekte yok. Lonca testinden önce kopyala.
- Uzak kabuk `sh`: `diff <(…)` gibi bash sözdizimi bütün komut satırını ayrıştırma aşamasında reddeder (hiçbir adım çalışmaz).
- `/set <ad> gold <n>` yangı n'e ayarlamaz, n ekler (`game/cmd_gm.cpp:1370-1375`).
- Giriş ekranında bekleyen client'ın bağlantıları `sockstat`'ta görünür; oyunda karakter olduğu anlamına gelmez.
