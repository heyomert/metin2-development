# A-12: kurulum ağacının izinleri ve sahipliği — kök neden analizi

**Durum: analiz (2026-10-06), salt okuma.** VM'de hiçbir izin, hesap ya da dosya değiştirilmedi. `perms.py`, VM izinleri,
`m2build` hesabı ve servisin kullanıcısı için değişiklikler ayrı etki analizi ve onayla gelir. Etiketler: **Kanıtlı**,
**Unverified**, **Öneri**. Yollar test VM'dedir (`ssh bsd`, root olarak girer).

## 1. Envanter (Kanıtlı, `stat`/`find`)
`/usr/local/m2dev-acceptance/server`: 19.543 öğe (+88 symlink), **hepsi `root:wheel`**.

| Mod | Sayı | Nerede |
|---|---|---|
| dosya `0666` | 7.227 | `share/data` (6.579), `share/locale`'in bir kısmı (1.156), `sql/`, kökteki `start.py`, `stop.py`, `channels.py`, `install.py`, `clear.py`, `perms.py` |
| dizin `0777` | 535 | `server/`, `share/`, `share/bin/`, `share/mark/`, `share/data/*`, `share/locale/*`… |
| dosya `0777` | 1 | `share/locale/english/quest/qc.exe` |
| `0644`/`0755` | 11.729 | `channels/` (kurulumda oluşturuldu), `share/locale`'in çoğu (quest derleme çıktısı), `share/bin/{game,db,qc}` (`0755`) |
| `0640`/`0750` | 38 | `share/conf` (2026-10-05'te elle düzeltildi) |
| `drwxrwx---` | 1 | `share/locale/english/quest/object` |

Derleme kaynağı `/usr/local/m2dev-acceptance/server-src`: 1.585 öğenin 1.582'si herkese yazılabilir. `COMPONENTS.lock` `0666`.
Üst dizinler (`/usr/local`, `/usr/local/m2dev-acceptance`) `0755`; yani yerel her kullanıcı bu ağaca ulaşabilir.

## 2. Kök neden
1. **Tek seferde, zaman damgası korunarak taşındı (Kanıtlı).** `share/data` altındaki 6.169 dosyanın oluşma zamanı aynı
   dakika (2026-10-04 11:51); VM'deki değiştirme zamanları Windows checkout'undaki dosyalarla birebir aynı (`start.py`,
   `perms.py`, `share/data`: 11:51).
2. **Mod deseni Windows `tar.exe` (bsdtar 3.5.2) ile birebir yeniden üretildi (Kanıtlı).** Aynı dosyalar Windows'ta
   `tar.exe` ile arşivlendiğinde: dosya `0666`, dizin `0777`, `qc.exe` `0777`. Git Bash'in GNU tar'ı ise `0644`/`0755`
   üretiyor. `/root/m2dev-server-snapshot.tar.gz` (sonradan alınmış anlık görüntü) içinde de modlar sadece `0666`, `0777`,
   `0777` (`.exe`).
3. **FreeBSD `tar` root olarak açarken arşivdeki modları umask uygulamadan korur** (bsdtar, root için `-p` varsayılan) →
   modlar olduğu gibi kaldı. VM'de sonradan oluşan her şey (kanal dizinleri, quest çıktısı, log'lar) root'un umask'ıyla
   `644`/`755`; ayrım bunu doğruluyor.
4. **İlk gerçek aktarımda kullanılan araç ve komut kayıtlı değil (Unverified).** Kanıtlar Windows bsdtar ile tutarlı;
   başka bir araç dışlanmadı.

### `perms.py`'nin payı (Kanıtlı, `server/perms.py`)
`perms.py` ağacın izinlerini **açıklamaz**; sadece şunları değiştirir:
- `share/bin/game` ve `share/bin/db` → `0777` (`perms.py:51-61`). VM'de çalışmamış: binary'ler `0755`.
- **`/var/db/mysql`'in alt dizinlerindeki bütün dosyalar → `0777`** (`perms.py:37-48`). VM'de çalışmamış: MariaDB
  dosyaları `0660 mysql:mysql`, dizinleri `0700`.
Dizinlere, script'lere, `share/data`'ya, `share/conf`'a dokunmaz. Upstream README binary'ler için `chmod 777` öneriyor
(`server/README.md:112-129, 749`).

**Ayrı risk — kesinlikle kabul edilmez:** `/var/db/mysql` üzerinde `0777`. Çalıştırılırsa bütün veritabanı dosyaları
(hesap tablosu, şifre özetleri, oyuncu verisi) her yerel kullanıcıya okunur ve yazılır olur. Ağaçtaki izinlerden bağımsız,
daha ağır bir tehlike.

## 3. Root olarak ne çalışıyor (Kanıtlı)
`/usr/local/etc/rc.d/m2dev` → `daemon -f /usr/local/bin/python3 start.py` (cwd `server/`) → `import channels` → kanal
dizinlerindeki symlink'lerle `share/bin/{game,db}`. `stop.py` de root ile çalışır. Bu zincirdeki yazılabilir halkalar:
- `start.py`, `stop.py`, `channels.py` (`0666`): değiştirilirse kod root olarak çalışır.
- `server/` (`0777`): Python script dizinini modül aramasında ilk sıraya koyar → buraya konan `json.py`/`subprocess.py`
  standart kütüphaneyi gölgeler.
- `share/bin/` (`0777`): binary'ler `0755` ama dizin yazılabilir → başka dosyayla değiştirilebilir.
- `share/data`, `share/locale` (`0666`/`0777`): oyun verisi (proto, quest, drop, spawn) herkese yazılabilir → güvenlik
  dışında **oyun verisi bütünlüğü / ekonomi** sorunu.
- `server-src` (`0666`/`0777`): root bu ağaçtan derleyip binary'yi devreye alıyor (`docs/build-and-run.md`) → derlemeye
  kod sokulabilir.

## 4. Runtime'ın gerçekten yazdıkları (Kanıtlı, kod)
Her süreç kendi çalışma dizinine (`channels/<…>/`) yazar: `log/` (`metrics_*`, `sql_*`, `syslog_*`), `syslog.log`,
`syserr.log` (`libthecore/log.cpp:43`), `pid` (`libthecore/main.cpp:18`), `version.txt`/`VERSION.txt`, `usage.txt` (db),
`castle_data.txt` (`game/castle.cpp:521`), `lotto.txt`, `special_item_group_vnum.txt`. **Paylaşılan tek yazma alanı
`share/mark`** (kanal dizinindeki `mark` symlink'i; lonca amblemleri, `game/MarkManager.cpp:39,115`). Başlatıcı
`server/pids.json` yazar. Süreçler root olduğu için bunların **hiçbiri** grup ya da herkes yazma izni gerektirmiyor.

## 5. Root zorunlu mu? (Kanıtlı, karar değil)
Kodda ayrıcalıklı çağrı yok (`setuid`, `chroot`, `setrlimit`… aranınca sadece `std::bind` çıktı). Dinleme portları 1024
üstü: game 11000–11991 ve 12000–12991, db 9000 (`sockstat`). **Root kodun ihtiyacı değil.** Servisi root dışı kullanıcıya
taşımak ayrı bir en-az-yetki tasarımıdır; bu analizin kapsamı değil.

## 6. Yerel hesaplar (Kanıtlı)
`nobody`, `m2stat` (nologin, dbstat; `docs/engineering/db-step1b-collector.md`) ve **`m2build`**: uid 1001, `/bin/sh`,
parolası var, **`wheel` grubunda** (FreeBSD'de `su` ile root'a geçiş bu grubu gerektirir), SSH anahtarı yok,
2026-10-04 16:31'de oluşturulmuş; ev dizininde sadece varsayılan dosyalar, başka sahip olduğu dosya ve crontab yok, giriş
kaydı yok. Dokümanlarda geçmiyor; **amacı kayıtlı değil (Unverified)**. SSH: `passwordauthentication no`,
`kbdinteractiveauthentication yes` (PAM üzerinden parola sorusu mümkün olabilir; **denenmedi**). VM sadece host-only ağda.
Herkese yazılabilir ağaç, giriş yapabilen her hesap için root'a giden bir yoldur. Not: analiz sırasında bu hesabın parola
özeti oturum çıktısına düştü (repoya yazılmadı) → parola ifşa olmuş kabul edilmeli.

## 7. Hedef sözleşme (Öneri; değişiklikler ayrı onayla)
**İlke:** root'un çalıştırdığı ya da okuduğu hiçbir şey, sadece yetkili derleme/deploy kimliği dışında yazılabilir olmaz;
runtime'ın yazdığı yerler açıkça tanımlıdır; doğru durum her yeni kurulumda ve deploy'da **kendiliğinden** üretilir ve
**doğrulanır**.

| Alan | Hedef |
|---|---|
| Kurulum ağacı (`server/`, script'ler, `share/data`, `share/locale`) | Yetkili deploy kimliğinin sahipliği; grup ve herkes yazamaz (dizin `0755`, dosya `0644`) |
| Çalıştırılanlar (`share/bin/{game,db,qc}`) | Aynı sahip, `0755` |
| `share/conf` | `0750`/`0640` (bugünkü); DB bilgileri |
| Runtime yazma (`channels/*/`, `log/`, `share/mark`, `pids.json`) | Sadece çalışan sürecin kimliği yazar (bugün root) |
| `server-src` (derleme girdisi) | Herkes yazamaz. **Doğru sahip henüz bağlanmadı:** derleme/deploy akışı ve `m2build`'in amacı önce doğrulanacak |
| `/var/db/mysql` | MariaDB varsayılanı (`mysql:mysql`, `0700`/`0660`); hiçbir script buraya mod uygulamaz |

**Mekanizma (Öneri):** (1) kurulum/deploy script'i arşivi modları normalleştirerek açar (arşivdeki modlara güvenmez),
çalıştırılacak dosyalar listesine `0755` verir; (2) `perms.py`'nin `0777` ve `/var/db/mysql` bölümleri kaldırılır ya da
script emekli edilir, upstream README talimatı bizim dokümanımızda geçersiz sayılır; (3) **salt okunur izin doğrulaması**
her deploy'un sonunda ve production kapısında çalışır, bozulan durumda başarısız olur (Faz 3'te güvenlik sinyali);
(4) tanımsız giriş hesabı olmaz.
