# Mimari

Son doğrulama: 2026-10-04 @ 5452c50f

Bu files projesinin parçalarının nasıl birleştiği. Bilgiler `yol:satır` kanıtı taşır;
kanıtlanmayanlar `Unverified` işaretlidir.

## Parçalar ve upstream

| Parça | Yol | Upstream / sabitlenmiş sürüm |
|---|---|---|
| Client | `client/` | `d1str4ught/m2dev-client` @ `ecef2dcd` |
| Client kaynağı | `client-src/` | `d1str4ught/m2dev-client-src` @ `a7555110` |
| Server | `server/` | `d1str4ught/m2dev-server` @ `51da0646` |
| Server kaynağı | `server-src/` | `d1str4ught/m2dev-server-src` @ `21519899` |

Kanıt: `COMPONENTS.lock`. 2026-10-04'te dört bileşenin içeriği upstream'le dosya dosya
karşılaştırıldı; tek fark büyük binary'lerin bu repoda LFS'de tutulması (`.gitattributes`).

## Süreçler ve topoloji

| Süreç | Klasör (VM) | Port / P2P | Rolü |
|---|---|---|---|
| db | `channels/db` | 9000 (sadece 127.0.0.1) | Proto `.txt`'leri yükler, oyuncu verisini önbellekler, MariaDB ile konuşur |
| auth | `channels/auth` | 11000 / 12000 | Giriş (`AUTH_SERVER: master`) |
| channel1_core1/2/3 | `channels/channel1/core{1,2,3}` | 11011–11013 / 12011–12013 | Oyun çekirdekleri |
| channel99_core1 | `channels/channel99/core1` | 11991 / 12991 | Özel haritalar |

- Port formülü: `11000 + kanal×10 + çekirdek`, P2P `12000 + …` (`server/install.py:27-29`).
- Tasarım: CH1–CH4 + CH99 (`server/channels.py:14-20`). Test VM'inde sadece CH1 + CH99 başlatılıyor (`m2dev_channels=1`).
- Bütün çekirdekler aynı `game` binary'sine symlink; klasör adı süreç adını belirliyor (VM'de `ls -la channels/channel1/core2`).

**Çekirdek başına haritalar** (`server/channels.py:2-10`, her kanal için aynı):

| Çekirdek | Haritalar | Kapsadığı |
|---|---|---|
| core1 | 1 4 5 6 3 23 43 112 107 67 68 72 208 302 304 | Shinsoo (başlangıç map 1) |
| core2 | 21 24 25 26 108 61 63 69 70 73 216 217 303 352 | Chunjo (başlangıç map 21) |
| core3 | 41 44 45 46 109 62 64 65 66 71 104 301 351 | Jinno (başlangıç map 41) |
| CH99 core1 | 113 81 100 101 103 105 110 111 114 118–128 181 182 183 200 | Özel/etkinlik haritaları |

Kontrol: üç imparatorluğun başlangıç haritası (1/21/41, `server-src/src/game/start_position.cpp:17-23`)
her kanalda bir çekirdek tarafından yükleniyor. Sonuç: **tasarımda tamam**. Bir çekirdek çalışmazsa
o imparatorluğun karakterleri login'e geri atılır (`docs/worklog/2026-10-04-channel-cores-map41.md`).

Başlatma/durdurma: upstream `server/start.py N` / `server/stop.py` (`pids.json` ile); VM'de bunları saran
`service m2dev`. Ayrıntılar `docs/build-and-run.md`'de.

## Client ↔ server

- Server listesi / IP / port: `client/assets/root/serverinfo.py:1-14`.
  - `SERVER_IP` ortama özel. Upstream değeri `178.18.255.214`, yerelde VM IP'si — **commit'lenmez**.
  - `SERVER_IP_TEST = "127.0.0.1"` → server seç ekranındaki "02. Test" bu bilgisayara bağlanır, kullanılmıyor.
- Giriş akışı: client → auth 11000 → kanal çekirdeği → karakterin haritası başka çekirdekteyse P2P ile o çekirdeğe warp.
  Harita hiçbir çekirdekte yoksa: `cannot find server for mapindex` (`GetServerLocation`) → login'e dönüş.
- Python UI kökü: `client/assets/root/`. Python'a export edilen C++ modülleri `client-src/src/UserInterface/Python*Module.cpp`
  (ör. `ime` → `PythonIMEModule.cpp:311`).

## Veri akışı

- **Item/mob proto'ları:** kaynak `server/share/conf/{item,mob}_proto.txt` + `*_names*.txt` → `db` açılışta yükler
  (`server-src/src/db/ClientManagerBoot.cpp:15-38`) → çekirdeklere bellekten gönderilir (`QUERY_BOOT`,
  `server-src/src/db/ClientManager.cpp:299-304`).
  SQL tabloları `player.item_proto` / `player.mob_proto`: **sadece kopya** (`Mirror*IntoDB`); kodda bu tablolardan okuma yok.
- **Quest'ler:** kaynak `server/share/locale/english/quest/*.quest` → `make.py` sadece `locale_list`'teki
  quest'leri `qc` ile derler (`make.py:71`) → `object/`. Listede olmayan quest dosyası aktif değildir.
- **Client paketleri:** giriş `client/assets/<klasör>` → `client/assets/PackMaker.exe` → `client/pack/<klasör>.pck`.
- **Veritabanları:** `account`, `common`, `player`, `log`, `hotbackup` (şemalar `server/sql/*.sql`).
- **Giriş kayıtları:** `log.loginlog2` — süre sadece menüden çıkışta yazılır (`server-src/src/game/cmd_general.cpp:277-286`);
  karakter verisi ise her bağlantı kopuşunda kaydedilir (`desc.cpp:107` → `CHARACTER::Disconnect`, `char.cpp:1333`).

## Yol sınıflandırması

| Yol ailesi | Sınıf | Not |
|---|---|---|
| `client-src/src`, `server-src/src` | source | |
| `client-src/CMakeLists.txt`, `server-src/**/CMakeLists.txt` | build-definition | `game` build'i `GLOB_RECURSE` |
| `server-src/src/common/*.h`, `client-src/src/UserInterface/Locale_inc.h` | shared-contract | flag'ler, sabitler, struct'lar |
| `client/assets/root/*.py` | source | Python UI |
| `server/share/conf/*.txt`, VM'de `channels/*/CONFIG` | runtime-config / static-data | |
| `server/share/locale/english/quest/*.quest` | quest-source | `locale_list` belirler |
| `server/sql/*.sql` | schema | |
| `client/Metin2.exe`, `client/assets/PackMaker.exe`, `server/share/bin/*` | compiled | kaynaktan yeniden üret |
| `client/pack/*.pck` | packed | `client/assets`'ten üretilir, commit'lenmez |
| `client/log/`, VM'de `syserr.log`/`syslog.log`, `pids.json` | runtime-state | |
| `client-src/vendor`, `client-src/extern`, `server-src/vendor` | vendor | |

## Feature flag'ler ve paketler (sadece konum)

- Server: `server-src/src/common/service.h`. Client: `client-src/src/UserInterface/Locale_inc.h`.
  İsimler iki tarafta farklı olabilir (ör. client'ta `ENABLE_DRAGON_SOUL_SYSTEM` var, server'da karşılığı flag değil).
- Paket tanımları: `server-src/src/common/` ve `client-src/src/UserInterface/`. Tam eşleme çıkarılmadı (Unverified).

## Doğrulanmayanlar

- Server binary'lerinin VM'de hangi komutla derlendiği (VM'de `/usr/local/m2dev-acceptance/build` var; komut kaydı yok).
- Client ↔ server paket ID eşlemesi.
