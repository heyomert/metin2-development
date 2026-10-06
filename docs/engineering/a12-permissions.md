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
1. **Arşivle, zaman damgaları korunarak taşındı (Kanıtlı).** VM'deki değiştirme zamanları Windows checkout'undaki
   dosyalarla birebir aynı (`start.py`, `perms.py`, `share/data`: 2026-10-04 11:51). Bu zamanlar **aktarım anı değil**:
   işletim sistemi 16:19–16:36 arasında kuruldu (`/var/log/bsdinstall_log`, ilk açılış 16:36), dosyaların "oluşma"
   zamanı (11:51) ondan önce görünüyor çünkü FreeBSD, açılan dosyaya eski bir zaman yazılınca oluşma zamanını da geri
   çeker. Gerçek açılma anı inode değişim zamanında: **16:52:05–06** (`stat` `ctime`). (Düzeltme, 2026-10-06: ilk
   sürüm aynı dakikadaki "oluşma" zamanını tek seferlik aktarımın kanıtı saymıştı; o dakika Windows checkout'unun anı.)
2. **Aktarım arşivi `/root/m2dev-server-snapshot.tar.gz` (Kanıtlı).** VM'de 16:52:04'te oluştu, ağaç 16:52:05'te açıldı;
   gzip başlığındaki sıkıştırma zamanı 16:49:26. İçindeki modlar sadece `0666`, `0777`, `0777` (`.exe`). (İlk sürüm bunu
   "sonradan alınmış anlık görüntü" diye yanlış adlandırmıştı.)
3. **Arşivin parmak izi: Windows'ta libarchive biçimi (Kanıtlı); üreten araç kesin bağlanmıyor.** Aynı dosyalar dört araçla
   arşivlenip karşılaştırıldı:

   | | gzip OS baytı | tar başlığı | uid / kullanıcı adı | mod alanı | modlar |
   |---|---|---|---|---|---|
   | VM'deki arşiv | 3 | `ustar\0` `00` | 0 / boş | `000666 ` | `0666`/`0777` |
   | Windows `tar.exe` (bsdtar 3.5.2, libarchive) | 3 | `ustar\0` `00` | 0 / boş | `000666 ` | `0666`/`0777`, `.exe` `0777` |
   | Git Bash GNU tar | 3 | farklı | 601751 / `mertw` | `0000644` | `0644`/`0755` |
   | FreeBSD `tar` (libarchive, Unix) | 3 | `ustar\0` `00` | 0 / **`root`** | `000666 ` | diskteki modlar |
   | Python `tarfile` | 255 | `ustar\0` `00` | 0 / boş | farklı | — |

   - **Kanıtlı:** biçim libarchive'e özgü; **boş kullanıcı adı** (Unix'teki libarchive `root` yazar) ve NTFS kaynaklı
     `0666`/`0777` mod deseni Windows'u gösteriyor.
   - **Kanıtlı:** test edilen araçlar içinde arşivle birebir uyuşan yalnız Windows `tar.exe` (libarchive).
   - **Kesin bağlanmıyor:** ilk arşivi tam olarak hangi programın ürettiği; aynı çıktıyı verebilecek, test edilmemiş Windows
     araçları (libarchive kullanan başka programlar dahil) dışlanamadı.
   - **Unverified:** arşivi kimin, hangi komutla oluşturduğu.
4. **FreeBSD `tar` root olarak açarken arşivdeki modları umask uygulamadan korur** (bsdtar, root için `-p` varsayılan) →
   modlar olduğu gibi kaldı. VM'de sonradan oluşan her şey (kanal dizinleri, quest çıktısı, log'lar) root'un umask'ıyla
   `644`/`755`; ayrım bunu doğruluyor.
5. **Kim ve hangi komutla (Unverified).** Zaman ve arşivin Windows/libarchive parmak izi kanıtlı; üreten program kesin
   bağlanmıyor (3. madde); çalıştıran kişi ya da otomasyon kayıtlı değil.

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
**FreeBSD kurulum programının "adduser" adımında oluşturulmuş** (Kanıtlı: `/var/log/bsdinstall_log` "Running
installation step: adduser"; `/var/log/userlog` 2026-10-04 16:31:22, ilk açılıştan önce). Ev dizininde sadece varsayılan
dosyalar; başka sahip olduğu dosya, crontab, çalışan süreç, oturum, soket yok; giriş kaydı yok; repo, rc/servis, cron ve
derleme/deploy akışında referansı yok (belgelenmiş derleme ve deploy root ile yapılıyor); `sudo`/`doas` kurulu değil, tek
yetki yolu `wheel` (`su`). **Bugün hiçbir bağımlılığı görülmedi; kurulumda neden eklendiği kayıtlı değil (Unverified).** SSH: `passwordauthentication no`,
`kbdinteractiveauthentication yes` (PAM üzerinden parola sorusu mümkün olabilir; **denenmedi**). VM sadece host-only ağda.
Herkese yazılabilir ağaç, giriş yapabilen her hesap için root'a giden bir yoldur. Not: analiz sırasında bu hesabın parola
özeti oturum çıktısına düştü (repoya yazılmadı) → parola ifşa olmuş kabul edilmeli. **2026-10-06: parola ile girişi
kilitlendi** (`pw lock m2build`; hesap, ev dizini, kabuk ve grup üyeliği değişmedi; servis yeniden başlatılmadı). Hesaba
yeniden ihtiyaç olursa eski parola açılmaz; yeni kimlik bilgisi ve en az yetkili rol ayrıca tasarlanır.

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
