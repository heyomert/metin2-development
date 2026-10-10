# Regresyon tabanı (gerçek client ile kabul senaryoları)

**Ne için:** oyuncu verisini, ekonomiyi, çekirdek/harita geçişini ya da SQL/önbellek katmanını etkileyen bir değişiklikten sonra
"eski garantiler hâlâ geçerli mi" sorusunu aynı yöntemle cevaplamak. Senaryolar ilk kez PR #22 (AsyncSQL 2a) kabulünde,
FreeBSD test VM'de gerçek client'larla koşuldu (2026-10-07; ayrıntı `docs/worklog/2026-10-07-pr22-acceptance.md`).

**Ne zaman:** her PR'da hepsi değil. `docs/engineering/change-impact.md` §7, değişikliğin dokunduğu garantilere göre gerekli
senaryo ailesini seçer. Seçilmeyen senaryo için PR'da `Etkilenmiyor — <neden>` yazılır.

**Sayılar eşik değildir.** Aşağıda "ölçülen" diye geçen süreler, bayt ve deneme sayıları o günün tek bir koşusunun
değerleridir; ürün sınırı ya da geçme ölçütü olarak kullanılmaz. Geçme ölçütleri niteldir: kayıp yok, çift yok, sahiplik
doğru, kuyruk boşaldı, süreç düşmedi.

## Ortak kurallar
- **Ortam:** yalnız test VM (`ssh bsd`). Kurulum `m2dev-install-binaries.sh --policy test-vm`; öncesinde servis durmuşken
  `m2dev-backup consistent`; rollback çifti `share/bin/.prev.*` SHA-256 ile doğrulanır (`docs/build-and-run.md`).
- **Kimlik:** test edilen binary'nin `BUILD`, `deploy.log`, `version.txt` ve telemetri `build=` alanları aynı commit'i gösterir.
- **Önce/sonra:** mümkün olan her senaryo, kabul edilmiş önceki binary ile de koşulur (`change-impact.md` §6 madde 3).
- **Kesinti:** yalnız `tools/acceptance/outage.sh` ile; MariaDB'yi süre sonunda kendisi geri başlatır, kanıtı toplar.
  Client'a "şimdi" sinyali, MariaDB kapandıktan **sonra** ve agent mesajını bitirerek verilir (çalışan bir komutun
  ortasında yazılan metin kullanıcıya ulaşmayabilir).
- **Kanıt kaynakları:** `log/syserr.log`, `log/metrics_*.log` (`late_pulses`, `work_max_us`, `iter_gap_max_us`, `hb_us`,
  `io_us`), `log/sql_*.log` (`q`, `cq`, `q_bytes`, `oldest_ms_max`, `retry`, `res_*`, SAVE sayaçları),
  `log/sql_failures_*.log`, db `syslog.log` (`[PLAYER_LOAD] … gold`), `log.log` tablosu (oyun olayları), `player`/`item`/
  `player_index` tabloları. Okuma: `tools/metrics/m2metrics.py` (`--sql`, `--failures`).
- **Gizlilik:** raporlarda SQL metni, parola/özet, IP yazılmaz; DB kullanıcı yetkileri `IDENTIFIED BY` filtrelenerek okunur.

## Senaryolar
Her senaryo: **Amaç · Ön koşul · Adımlar · Client gözlemi · Sunucu/DB kanıtı · PASS · Kanıtlamadığı.**

### RB-01 Giriş / karakter seçimi / haritaya giriş
- **Amaç:** temel oturum akışı. **Ön koşul:** 6/6 süreç, kimlik doğrulandı.
- **Adımlar:** giriş → karakter seç → haritaya gir.
- **Client:** oyuna giriş, takılma yok. **Kanıt:** `ENTERGAME … map_index`, çekirdekte `users_local` artar, syserr'de yeni hata yok.
- **PASS:** giriş tamamlanır; syserr'de açıklanmamış yeni satır yok. **Kanıtlamadığı:** yük altında giriş.

### RB-02 Karakter oluşturma (başarı)
- **Amaç:** yeni karakterin doğru id ve hesap slotuna bağlanması. **Ön koşul:** hesapta boş slot.
- **Adımlar:** karakter oluştur (benzersiz ad), oyuna gir.
- **Kanıt:** `player` yeni satır, id önceki en yüksekten büyük; `player_index` ilgili `pidN` = bu id; diğer slotlar değişmedi.
- **PASS:** id ve slot doğru, başka hesabın/karakterin id'si yok. **Kanıtlamadığı:** eşzamanlı oluşturma.

### RB-03 MariaDB kapalıyken karakter oluşturma (güvenli başarısızlık)
- **Amaç:** başarısız oluşturmanın başarı ya da yanlış id üretmemesi (A-17). **Ön koşul:** RB-02 geçti.
- **Adımlar:** oluşturma formunu hazırla (onaylama) → MariaDB'yi durdur → "şimdi" → onayla → MariaDB'yi başlat.
- **Client:** "oluşturulamaz" mesajı. **Kanıt:** db syslog `GD::PLAYER_CREATE`; defterde `select.player_index … not_delivered`;
  `player`'da o ad yok; en yüksek id ve `player_index` değişmedi.
- **PASS:** satır yok, stale id yok, slot değişmedi. **Kanıtlamadığı:** INSERT anında düşen DB (ilk okuma önce düşer; bu yol
  `tools/sql-reliability` D4/D5 ile kanıtlı).

### RB-04 Normal ticaret (iki client)
- **Amaç:** item sahipliği ve yang korunumu. **Ön koşul:** iki hesap, iki karakter yan yana.
- **Adımlar:** A → B bir item (yığını bölmeden) + yang; iki taraf onaylar; ikisi de çıkıp girer.
- **Kanıt:** `log.log` `EXCHANGE_GIVE`/`EXCHANGE_TAKE` her ticaret için birer kez (yang yalnız > 1000 ise loglanır,
  `game/exchange.cpp:477`); `item` satırı tek ve `owner_id` = B; toplam item sayısı değişmedi; çift kimlik yok
  (`GROUP BY id HAVING COUNT(*) > 1` boş); yang: db `[PLAYER_LOAD] … gold` önce/sonra ve önbellek yazıldıktan sonra `player.gold`.
- **PASS:** item yalnız B'de, kayıp/çift yok, A'nın eksiği = B'nin artışı. **Kanıtlamadığı:** yığın bölme, iptal edilen ticaret.

### RB-05 Depo (safebox)
- **Amaç:** depo üzerinden aynı hesabın karakterleri arasında item aktarımı. **Adımlar:** karakter 1 koyar, karakter 2 alır.
- **Kanıt:** `SAFEBOX PUT/GET` logları; `save_safebox_err=0`; item'ın son sahibi ve penceresi doğru.
- **PASS:** item kaybı/çifti yok. A-19 (PR-2) sonrası aynı hesabın yeni karakteri ücret ödemez ve `insert.safebox` 1062
  **görülmez**; görülürse regresyondur (öncesinde beklenen bir satırdı). Hesap aktivasyonu senaryoları:
  `docs/engineering/safebox-activation.md` bölüm 8.

### RB-06 Kayıt ve tekrar giriş kalıcılığı
- **Amaç:** önbellekteki verinin MariaDB'ye yazılması. **Adımlar:** işlemlerden sonra çık → gir → aynı karakteri yükle.
- **Kanıt:** SAVE sayaçlarında `_err=0`; db `[PLAYER_LOAD]` yang; önbellek yazıldıktan sonra (`save_player_ok` artar)
  `player.gold`/konum/seviye aynı.
- **PASS:** client'taki değerler = yükleme kaydı = DB satırı. **Kanıtlamadığı:** db çökmesinden sonra (önbellekteki son
  dakikalar, `docs/backup.md`).

### RB-07 MariaDB kesintisi, oyuncu bağlıyken (boşta / yükleme)
- **Amaç:** geçici bağlantı hatasında kayıpsız, sıralı tekrar. **Adımlar:** `outage.sh --seconds <n>`; istenirse kesinti
  anında karakter seç ("Başla").
- **Kanıt:** 6/6 süreç ve PID'ler aynı; syserr'de bağlantı başına durum değişiminde birer satır (değer yok); `retry` artar,
  `stuck_conns`/`oldest_ms_max` büyür, DB dönünce `ok` artar ve `q`/`cq`/`q_bytes` kesinti öncesi düzeyine iner;
  `res_*` ve defter artışı yok.
- **PASS:** kayıp yok, kalıcı hata yok, kuyruk boşaldı, süreç düşmedi. **Kanıtlamadığı:** çok uzun kesinti (A-18 RST sınırı).

### RB-08 Aktif savaş sırasında kesinti
- **Amaç:** oyun döngüsünün DB'yi beklememesi. **Adımlar:** kesinti boyunca mob/Metin'e vur, skill/iksir kullan, hareket et.
- **Kanıt:** oyuncunun çekirdeğinde `work_max_us`/`iter_gap_max_us`/`late_pulses` kesinti öncesiyle aynı düzey; bütün
  çekirdeklerde aynı anda büyüyen aralık + küçük iş = VM, döngü değil (`docs/monitoring.md`).
- **PASS:** client'ta kesinti hissedilmez; döngü ölçümleri kesintisiz pencerelerle aynı düzey. **Kanıtlamadığı:** yoğun yük.

### RB-09 Mob ölümü / drop / toplama (kesinti sırasında)
- **Kanıt:** `GET`/`GET_GOLD` logları DB dönünce yazılır (zamanı yazıldığı an, `NOW()`); toplanan item kimlikleri
  `item`'da, sahibi doğru; çift kimlik yok.
- **PASS:** toplanan her item tek satır ve doğru sahipte.

### RB-10 Item / EXP / yang kalıcılığı (kesinti sonrası)
- **Adımlar:** DB dönünce envanter/EXP/yang kontrol → çık → gir.
- **Kanıt:** RB-06 ile aynı; bilinçli GM işlemleri (ör. `advance`) komut kaydıyla ayrılır.
- **PASS:** değerler korunur.

### RB-11 Kesinti sırasında çekirdekler arası warp
- **Amaç:** çekirdek değişiminin kesintide kontrollü davranması. **Ön koşul:** hedef haritanın başka çekirdekte olduğu
  `CONFIG`'teki `MAP_ALLOW` ile doğrulanır.
- **Adımlar:** MariaDB kapalıyken `goto`.
- **Client:** warp bekler, DB dönünce tamamlanır. **Kanıt:** `WarpSet … current map A target map B`, eski çekirdekte
  `P2P: Logout`, yeni çekirdekte `LOGIN_BY_KEY` → DB dönünce `LoginSuccess` → `player_load` → `ENTERGAME … map_index B`
  hedef koordinatla.
- **PASS:** doğru harita ve konum, kopma/geri atma yok, tekrar girişte aynı. **Kanıtlamadığı:** client'ın bekleme sınırı
  (uzun kesinti, roadmap A-22).

### RB-12 MariaDB kapalıyken iki client ticaret
- **Adımlar:** RB-04, MariaDB kapalıyken; ticaret penceresi "şimdi"den sonra açılır.
- **Kanıt:** core syslog `Exchange … ACCEPT` zamanları kesinti içinde; RB-04'ün bütün kanıtları DB dönünce ve önbellek
  yazıldıktan sonra.
- **PASS:** RB-04 ile aynı.

### RB-13 Kapanış (shutdown) boşaltması
- **Adımlar:** oyuncu yokken `service m2dev stop`.
- **Kanıt:** db `reason=final` satırlarında `q=0 cq=0 unexecuted_at_quit=0 q_bytes=0`; game çekirdeklerinde "End of pid";
  db syserr'de yalnız bilinen satırlar.
- **PASS:** çalıştırılmayan mesaj yok ya da her biri sayaç + defterde. **Kanıtlamadığı:** game'in kapanış özeti (log
  kapandıktan sonra yazılır; sayaç/defter yeterli).

### RB-14 Log güvenliği ve hata defteri
- **Kanıt:** kurulumdan sonraki bütün AsyncSQL/`[SLOW-*]` satırlarında SQL metni, tırnak, test karakter/hesap/lonca adları ve
  IP sayısı 0; defter dosyası `0600`; defter satırı sayısı = `res_*` artışı; `ledger_dropped`/`ledger_write_errors` 0
  (değilse açıklanır).
- **PASS:** sızıntı 0, defter sayaçlarla tutarlı. **Kanıtlamadığı:** defterin yük altındaki kapasitesi.

### RB-15 İsim değiştirme (Tincture of the Name, 71055)
- **Amaç:** item ve bekleme süresi yalnız ad gerçekten değiştiğinde tüketilir; ad listede, oyunda ve DB'de tutarlı (A-17 g19,
  `docs/worklog/2026-10-10-change-name-a17.md`).
- **Ön koşul:** seviye ≥ 35, loncasız, partisiz, evli değil, `chagne_name.next_time` dolmamış bir test karakteri; item GM
  `/item 71055` ile (test karakterine geçici `common.gmlist` satırı; testten sonra kaldırılır). Messenger etkisini görmek
  için karaktere iki yönlü bir `messenger_list` fixture'ı.
- **Adımlar:** (a) DB açıkken yeni adla kullan; (b) kullanılan bir adla kullan; (c) isim kutusu açıkken `outage.sh`, MariaDB
  kapandıktan sonra adı yaz. Her birinden sonra çık → karakter listesi → gir; (c)'de ayrıca 10 dk bekleyip (`g_iLogoutSeconds`,
  `db/Main.cpp:34`, önbellek boşalır) tekrar gir.
- **Kanıt:** `player.name`, `player.item` (vnum 71055), `messenger_list`, `log.change_name`, `log.log` (`CHANGE_NAME`),
  `player.quest` (`chagne_name.next_time`), syserr `CHANGE_NAME: failed pid=… step=… result=…`, hata defteri
  (`select.player`/`update.player`).
- **PASS:** (a) ad listede ve oyunda yeni, DB yeni, item gitti, messenger temizlendi, log ve bekleme süresi yazıldı.
  (b) "name is not available", hiçbir şey değişmedi. (c) "could not be changed" mesajı, item ve messenger duruyor, bekleme
  süresi ve `change_name` log satırı yok, ad listede, oyunda ve DB'de eski.
- **Kanıtlamadığı:** AMBIGUOUS (sorgu gönderildi, cevap okunamadı) uçtan uca üretilemiyor; karar mantığı
  `tools/change-name/test-change-name-logic.cpp` ile. Aynı anda aynı adı seçen iki oyuncu (g18 yarışı, ayrı iş).

## İlk koşu özeti (PR #22, 2026-10-07; tekrar ölçüt değil)
RB-01…RB-14 PASS. Bulunan ve 2a'dan bağımsız olanlar roadmap A-19…A-22'de.
