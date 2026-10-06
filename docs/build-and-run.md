# Derleme ve Çalıştırma

Son doğrulama: 2026-10-04 @ 5452c50f

Kesin, test edilmiş adımlar. Gizli bilgiler buraya asla yazılmaz — sadece nerede durdukları.

## Client

### Client exe'yi derle (Windows, VS 2022 Build Tools)
```powershell
$cm = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd client-src
& $cm -S . -B build -G "Visual Studio 17 2022" -A x64
& $cm --build build --config Release -- /m
```
- Bu makinede `-- /m` (paralel) bellek yetmezliğiyle düşüyor (C1060 "derleyicinin yığın alanı kalmadı", "disk belleği dosyası
  çok küçük"); kodda hata yokken. `-- /m:1` ile derle (2026-10-05'te bu şekilde 0 hatayla geçti).
- Çıktılar: `client-src/build/bin/Release/Metin2_Release.exe`, `config.exe`, `PackMaker.exe`, `DumpProto.exe`.
- `config.exe` → `client/config.exe` olarak kopyala (ayar programı, kaynağı `client-src/src/Config`; `config/metin2.cfg`'yi yazar).
- `Metin2_Release.exe` → `client/Metin2.exe` olarak kopyala (2026-10-04'te bu şekilde derlenip test edildi, ~5 dk).
- **Kural:** dağıtılan exe her zaman `client-src`'den derlenir. Upstream'in hazır exe'si kaynaktan eski kaldı
  (`docs/worklog/2026-10-04-login-input-secret-mode.md`).
- `client-src/build/` commit'lenmez (`client-src/.gitignore`).

### Asset'leri paketle
- Giriş `client/assets/<klasör>` → çıkış `client/pack/<klasör>.pck` (`client/assets/pack.py:8`).
- **Tuzak 1:** `python pack.py --all` klasörleri paralel paketliyor ve PackMaker sessizce hiçbir şey üretmiyor
  (2026-10-04'te 91 klasörden 1'i üretildi). Klasörleri **sırayla** paketle.
- **Tuzak 2:** PackMaker Git Bash'ten çalıştırılınca çıktı üretmiyor; **PowerShell**'den çalıştır.
- Tek klasör: `cd client/assets; .\PackMaker.exe --input root --output ..\pack`
- Hepsi, sırayla: `C:\Users\mertw\.m2dev\vm-scripts\pack-client-assets-serial.ps1` (`client/assets` içinden çalıştır).
- `client/pack/` commit'lenmez (`.gitignore`).

### Client'ı bir sunucuya yönlendir
- `client/assets/root/serverinfo.py` → `SERVER_IP`. Test için `192.168.56.20`. **Commit'lenmez.**
- Değiştirdikten sonra `root` klasörünü yeniden paketle.
- Server seç ekranında **"01. Metin2"** seçilir. "02. Test" `127.0.0.1`'e gider, kullanılmıyor.

## Server (test VM)

Sunucu dizini: `/usr/local/m2dev-acceptance/server` (repo `server/` + derlenmiş binary'ler).

### Başlat / durdur / durum
```sh
service m2dev status
service m2dev stop
service m2dev start
```
- Servis dosyası: `/usr/local/etc/rc.d/m2dev`, yedeği repo dışında `C:\Users\mertw\.m2dev\vm-scripts\m2dev.rc`.
- Upstream `start.py <kanal sayısı>` / `stop.py`'yi sarar. `start.py`'yi `daemon(8)` ile ayrı bir oturumda
  çalıştırır, çünkü açılışta konsol SIGHUP'ı sunucuyu öldürüyordu (`docs/worklog/2026-10-04-vm-autostart-sighup.md`).
- `/etc/rc.conf`: `m2dev_enable="YES"`, `m2dev_channels="1"`. Açılışta otomatik başlar (2026-10-04'te yeniden başlatmayla doğrulandı).
- `start.py` çıktısı: `/var/log/m2dev.log`. Süreç PID'leri: `/usr/local/m2dev-acceptance/server/pids.json` (VM'de, `start.py` yazar).
- Süreç log'ları: `channels/<süreç>/syserr.log`, `syslog.log`. **`syserr.log` her açılışta sıfırlanır.**
- Durdurmadan önce oyunda kimse olmadığını kontrol et:
  `sockstat -4c | grep -E ":(1101[1-3]|11991|11000) " | grep -v 127.0.0.1`

### Güvenlik duvarı (`pf`)
- Kural dosyası repoda: `deploy/freebsd/pf.conf` (LF satır sonu şart: `.gitattributes` → `deploy/** text eol=lf`). VM'de `/etc/pf.conf`.
- Ne yapar: çekirdekler arası **P2P portlarına** (12000–12999) sunucu dışından gelen TCP'yi düşürür. Kodda P2P'nin kimlik
  doğrulaması yok (`docs/roadmap.md` K-1). Geri kalan trafik (SSH, oyun portları, db) etkilenmez.
- `/etc/rc.conf`: `pf_enable="YES"`, `pf_rules="/etc/pf.conf"`. Açılışta kendiliğinden yüklenir (2026-10-05'te yeniden başlatmayla doğrulandı).
- Durum / kural / sayaç: `pfctl -s info`, `pfctl -vsr` (sayaçta engellenen paket sayısı görünür).
- Kuralı değiştirirken: önce `pfctl -nf /etc/pf.conf` (sözdizimi), sonra otomatik geri alma zamanlayıcısı kur
  (`daemon -f -p /var/run/pf-rollback.pid sh -c 'sleep 300; pfctl -d'`), yeni SSH bağlantısı çalışınca iptal et.
- Doğrulama (dışarıdan, host'tan): `Test-NetConnection 192.168.56.20 -Port 12011` → `False`, `-Port 11011` ve `-Port 22` → `True`.
- **Production'da aynı kural gerekli:** `docs/production-checklist.md` K-1. Ayrıntı: `docs/worklog/2026-10-05-p2p-firewall.md`.

### Config dosyaları ve yönetim kanalı
- VM'de `server/share/conf/` (çekirdeklerin `conf` symlink'i buraya bakar) dosyaları `640`, klasör `750` (2026-10-05). Bütün süreçler `root`.
  `server/perms.py` her şeyi `0o777` yapar; **yeniden çalıştırma**, çalıştırırsan izinleri geri düzelt (`docs/roadmap.md` A-12).
- `game.txt` → `ADMINPAGE_PASSWORD`: VM'de rastgele değer; kopyası VM'de `/root/.m2dev-admin-channel-password`, host'ta
  `C:\Users\mertw\.m2dev\secrets\admin-channel-password`. Repodaki `game.txt`'de yer tutucu var — repodan kopyalarsan değeri yeniden ayarla.
- `ADMINPAGE_IP: 127.0.0.1` — boş bırakma (boşsa şifreyi bilen herkes yönetici olur).
- Config değişikliği için `service m2dev` yeniden başlatılmalı (çekirdekler config'i açılışta okur).
- Şifreyi değiştirirken ya da kontrol ederken değeri ekrana basma; özet (`sha256`) ve sayım kullan. Ayrıntı: `docs/worklog/2026-10-05-admin-channel-config.md`.

### Veritabanı (MariaDB 11.8)
- **Zorunlu ayar:** `sql_mode=NO_ENGINE_SUBSTITUTION` (upstream şartı: `server-src/README.md:1078`).
  VM'de `/usr/local/etc/mysql/conf.d/zz-acceptance.cnf` içinde.
- Bu dosya **644** olmalı. MariaDB `mysql` kullanıcısıyla çalışır, `600` izinli dosyayı sessizce atlar
  (`docs/worklog/2026-10-04-mariadb-sql-mode.md`).
- Doğrulama: `mysql -e "SELECT GLOBAL_VALUE, GLOBAL_VALUE_ORIGIN FROM information_schema.SYSTEM_VARIABLES WHERE VARIABLE_NAME='SQL_MODE'"`
  → `NO_ENGINE_SUBSTITUTION | CONFIG`.
- `bind-address=127.0.0.1` bilinçli olarak böyle (upstream `0.0.0.0` diyor; burada sadece VM içinden erişim).
- **Giriş artık `PASSWORD()` kullanmıyor (K-2 B, 2026-10-05):** şifre özeti `game` içinde hesaplanıyor
  (`server-src/src/game/utils.cpp` → `mysql_native_password_hash`, saklanan `*SHA1(SHA1)` biçimiyle aynı). SQL'e şifre gitmiyor,
  `old_passwords` ayarı girişi etkilemiyor. Boş şifre sunucuda reddediliyor. Ayrıntı: `docs/worklog/2026-10-05-k2-password-review.md`.
- Şemalar: `server/sql/*.sql`. Şema değişikliği = `server/sql` dosyası + mevcut DB için migration SQL (PR açıklamasında).
- **Yedek:** saatlik sıcak yedek (cron), bakım öncesi `m2dev-backup consistent`, günlük çekme + geri yükleme testi. Kurulum,
  felakette geri yükleme ve durum dosyaları: `docs/backup.md`. Veritabanına elle dokunmadan önce bakım yedeği al.

### Quest derleme
- `server/share/locale/english/quest/make.py` sadece `locale_list`'teki quest'leri `qc` ile `object/`'e derler (`make.py:71`).
- VM'de nasıl çalıştırıldığı kayıtlı değil (Unverified). VM'de `/root/m2dev-acceptance-quest-build.log` var.

### Server binary'lerini derleme (VM)
Doğrulandı 2026-10-05: sıfırdan yapılandırma 12 sn, derleme 116 sn (`-j4`), 0 hata, 372 uyarı.
```sh
cmake -S /usr/local/m2dev-acceptance/server-src -B /root/build-verify -DCMAKE_BUILD_TYPE=Release
cmake --build /root/build-verify -j4
```
- Çıktı: `<build>/bin/{game,db,qc}`. Derleme çalışan sunucuya **dokunmaz** (çıktı sadece `bin/`'e gider, `server-src/CMakeLists.txt:46`;
  kopyalama/install adımı yok).
- Çalışan binary'ler: `/usr/local/m2dev-acceptance/server/share/bin/{game,db,qc}` — kanal klasörlerindeki `channelN_coreM` ve `db`
  bunlara symlink. Devreye almak = `service m2dev stop` → **`m2dev-backup consistent`** (kayıpsız DB yedeği; servis çalışırken
  reddeder, `docs/backup.md`) → **`m2dev-install-binaries.sh`** (game + db birlikte, aşağıda) → servisi başlat → test.
- VM kaynak ağacı `/usr/local/m2dev-acceptance/server-src`, repodaki `server-src` ile aynı (460 dosyanın içerik özeti eşleşti).
  Kod değiştirirken önce repodaki değişikliği VM'e taşı, sonra derle.
- **İki derleme dizini var, farkı bil:**
  - `/usr/local/m2dev-acceptance/build/server-freebsd`: 4 Ekim derlemesi. Sunucu kodu `-O3`, ama vendor MariaDB kütüphanesi
    `-O2 -g` ile derlenmiş (`libmariadbclient.a` 3,5 MB, debug bilgili). Sebebi kayıtlı değil. **Artık kullanılmıyor.**
  - `/root/build-verify`: yukarıdaki komutla sıfırdan; her şey `-O3`, MariaDB kütüphanesi debug bilgisiz (0,9 MB). **Bundan sonra burada derle.**
- **Şu an çalışan binary'ler (2026-10-06):** `game` ve `db` → DB adım 1c (PR #17, `/root/build-1c`, kaynak `/root/src-1c`);
  `qc` → 4 Ekim derlemesi. Önceki çift `/root/pre-1c-2026-10-06/` (sha256 ile). Bu binary'lerin gömülü kimliği yok
  (derleme kimliğinden önce derlendi).
- VM kaynak dosyaları: değiştirdiğin dosyayı VM'e LF olarak kopyala (`tr -d '\r' < dosya | ssh bsd "cat > hedef"`), sonra özetleri
  karşılaştır. VM'deki diğer kaynak dosyalar CRLF (Windows checkout'tan); derleyici için fark etmez.
- Debug bilgisi kararı: çökme analizinde (roadmap 1.7) debug bilgili binary işe yarar. `Release` debug bilgisi içermiyor;
  `RelWithDebInfo` ya da ayrı sembol dosyası değerlendirilmeli — henüz karar verilmedi.
- Yedekler (VM): 4 Ekim binary'leri ve özetleri `/root/build-baseline-2026-10-05/`; K-3 öncesi kaynak dosyaları `/root/k3-code-backup/`.
  Geri dönüş: servisi durdur → `cp /root/build-baseline-2026-10-05/game /usr/local/m2dev-acceptance/server/share/bin/game` → başlat.

### Derleme kimliği ve kurulum (roadmap 1.9 / T-2)
Her game ve db binary'si hangi kaynaktan derlendiğini kendi içinde taşır; kurulum bu kimliği ve dosyanın SHA-256'sını
kaydeder. Kod: `server-src/cmake/BuildIdentity.cmake`, `src/common/build_identity*.h`, `deploy/freebsd/m2dev-install-binaries.sh`.

**Kimlik her derlemede hesaplanır** (configure anında değil); değişmediyse başlık yazılmaz, hiçbir şey yeniden derlenmez;
değiştiyse sadece iki `version.cpp` derlenir (ölçüldü). Kaynak, ilk uyan:

| `src` | Ne zaman | `commit` / `dirty` |
|---|---|---|
| `git` | `server-src` bir git çalışma ağacında | `HEAD` / `server-src` altında git'in gördüğü her şey: değişmiş, izlenmeyen **ve yok sayılan** dosyalar (`GLOB_RECURSE` yok sayılan `old_BK/*.cpp`'yi de derler), bu derlemenin kendi derleme dizini hariç. `client/` sayılmaz |
| `archive` | `server-src/SOURCE_COMMIT` 40 hex (`git archive` + `export-subst`) | dışa aktarılan commit / `0` (açıldıktan sonraki değişikliği **göremez**) |
| `injected` | `-DM2_BUILD_COMMIT=<40 hex> -DM2_BUILD_DIRTY=0\|1` | verilen değer (doğrulanmamış beyan) |
| `none` | hiçbiri | `unknown` + derlemede görünür CMake uyarısı; derleme sürer |

Çalışırken: açılışta `version.txt` (game) / `VERSION.txt` (db) `component=… commit=… dirty=… src=… describe=…` ve syslog'da
`BUILD:` satırı; sağlık ve SQL `sum` satırlarında `build= build_dirty= build_src=` (`docs/monitoring.md`).

**Kurulum politikası** (`m2dev-install-binaries.sh`, varsayılan `production`, fail-closed):

| Kimlik | Geliştirme (installer yok) | `--policy test-vm` | `--policy production` |
|---|---|---|---|
| `git`, `dirty=0` | ✓ | ✓ | ✓ (**sadece bu**) |
| `git`, `dirty=1` | ✓ + uyarı | sadece `--policy dev` | ✗ |
| `archive` | ✓ | ✓ | ✗ |
| `injected` | ✓ + uyarı | sadece `--policy dev` | ✗ |
| `none` / `unknown` | ✓ + CMake uyarısı | sadece `--policy dev` | ✗ |
| işaret yok/bozuk/çift, alanları `src` ile tutarsız, game ≠ db (commit/dirty/src), hedeften çalışan game/db süreci | — | ✗ (`dev` dahil) | ✗ |

İşaretin alanları kaynak türüyle tutarlı olmalı, `BuildIdentity.cmake`'in ürettiği gibi: `git` → 40 hex commit, `dirty`
`0|1`; `archive` → 40 hex, `dirty=0`; `injected` → 40 hex; `none` → `commit=unknown dirty=unknown`. Sözdizimi tek başına
yetmez (ör. `src=git commit=unknown` biçimce geçerli ama tutarsız; her politikada reddedilir). `test-vm` ve `production`
`commit=unknown`'ı ayrıca, tutarlılık kontrolünden bağımsız olarak da reddeder.

Her politikada: iki binary önce tamamen doğrulanır (hiçbir dosyaya dokunmadan), sonra aşama → mevcut çifti yedekle
(`share/bin/.prev.<zaman>.<pid>`) → `rename` → SHA-256 doğrula → `share/bin/BUILD` + `/var/db/m2dev/deploy.log`. İkinci dosya,
doğrulama ya da kayıt adımı başarısız olursa önceki çift (ve `BUILD`) geri konur ve doğrulanır; `result=installed` sadece
hepsi başarılıysa yazılır (`result=failed rolled_back=1` denetim için). Süreç kontrolü `procstat -b -a` ile gerçek binary
yolundan yapılır (`service m2dev status`'a güvenilmez, T-4). Modlar: binary `0755`, `BUILD` `0644`, `deploy.log` `0640`,
dizinler `0750`/`0700`; herkese yazılabilir bir şey oluşturulmaz. Testler: `deploy/freebsd/test-install-binaries.sh`.

**Kanıtın sınırı:** gömülü işaret = binary'nin kendi kaynak kimliği **beyanı**; SHA-256 = kurulan **tam dosya**; temiz git
derleme yolu = kaynak ağacının **doğrulanmış** durumu. Hiçbiri tek başına imzalı derleme/özgünlük kanıtı değildir; işaret
taklit edilebilir. A-12 izin/deploy bütünlüğü bu tehdidi azaltır; imzalı derleme gerekirse ayrıca değerlendirilir.

**Production yolu (henüz yok):** git'li bir FreeBSD derleme makinesinde, istenen commit'i temiz bir dizine klonlayıp ayrı
dizinde derleyen, derlemeden önce ve sonra `git status`'u yeniden kontrol eden ve işaretin `HEAD` ile aynı olduğunu
doğrulayan tek giriş noktalı release script'i. Kurulmadan ve fiilen test edilmeden production kapısı kapanmaz.

**Test VM bugün (git yok) — `archive` yolu:**
```sh
# Windows (repo kökü): commit'lenmiş içerik; tar.umask=022 -> 0644/0755, herkese yazılabilir dosya yok
git -c tar.umask=022 archive --format=tar.gz -o server-src-<commit>.tar.gz <commit> server-src
# VM: her seferinde temiz dizin; -m dosyalara açılma zamanını verir (aşağıdaki saat farkı)
rm -rf /root/src-<commit> && mkdir /root/src-<commit> && tar -xzmf server-src-<commit>.tar.gz -C /root/src-<commit>
cmake -S /root/src-<commit>/server-src -B /root/build-<commit> -DCMAKE_BUILD_TYPE=Release && cmake --build /root/build-<commit> -j4
sh m2dev-install-binaries.sh --policy test-vm /root/build-<commit>/bin/game /root/build-<commit>/bin/db
```
Tuzaklar (ölçüldü 2026-10-06): (1) Windows saati VM'den ~4,7 dk ileride (VM'de `ntpd` kapalı); `git archive` dosya
zamanlarını commit zamanı yazar → `-m` olmadan açılan dosyalar VM'e göre "gelecekte" kalır ve make o süre boyunca her şeyi
yeniden derler. (2) FreeBSD `make` zamanları saniye çözünürlüğünde karşılaştırır; kimlik başlığı bu yüzden sadece
değiştiğinde ve içinde bulunulan saniye geçtikten sonra yazılır (`BuildIdentity.cmake`).

## Test ortamı

- VM: VirtualBox `M2DEV_FREEBSD151_ACCEPTANCE` (FreeBSD 15.1), host-only IP `192.168.56.20`.
- Erişim: `ssh bsd` (`~/.ssh/config`, anahtar tabanlı). Yedek anahtar ve VM root şifresi repo dışında
  `C:\Users\mertw\.m2dev\secrets\`.
- **Dikkat:** `ssh vm` (127.0.0.1:10022) başka bir VM'dir (MARTYSAMA), bu projeyle ilgisi yok.
- GM test hesabı: login `admin`, karakter `[SA]Admin`, `IMPLEMENTOR` (`common.gmlist`). Şifre sadece geliştirme
  amaçlı, VM'de `/root/.m2dev-acceptance-test-password` içinde. `common.gmhost` = `*.*.*.*` (production'da değişmeli).
- Diğer hesap: `test` (karakter `Test`, GM değil).
- 2026-10-04'te alınan yedekler (VM `/root`): `loginlog2.before-is_gm-fix.sql`, `loginlog2.before-playtime-fix.sql`,
  `zz-acceptance.cnf.bak`, `m2dev.rc.bak`.

## Hızlı sorun giderme

| Belirti | İlk kontrol | Ayrıntı |
|---|---|---|
| Login kutularına yazı yazılamıyor | `client/log/syserr.txt` → `AttributeError: module 'ime' ...` | `docs/worklog/2026-10-04-login-input-secret-mode.md` |
| "Oyunu başlat" sonrası login'e dönüş | Kanal çekirdeğinin `syserr.log`'unda `cannot find server for mapindex` | `docs/worklog/2026-10-04-channel-cores-map41.md` |
| VM açıldı, sunucu yok | `service m2dev status`, `/var/log/m2dev.log` | `docs/worklog/2026-10-04-vm-autostart-sighup.md` |
| db açılışta "Data too long / truncated" | `sql_mode`'un kaynağı `CONFIG` mı | `docs/worklog/2026-10-04-mariadb-sql-mode.md` |
| `pack/` boş ya da eksik | Paralel paketleme / Git Bash | Bu dosya → "Asset'leri paketle" |
