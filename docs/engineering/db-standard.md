# Veritabanı standardı ve InnoDB'ye geçiş — analiz ve test planı

**Durum: TASLAK, onay bekliyor (2026-10-06).** Kod, şema ve canlı veri bu analiz sırasında değiştirilmedi. Deneyler
geçici MariaDB örneklerinde (bağlanılan `@@datadir` kontrol edilerek) ve salt okunur kod incelemesiyle yapıldı.
Etiketler: **Kanıtlı** (kod `yol:satır`, komut çıktısı ya da ölçüm), **Bilinmiyor** (ne kontrol edildiği yazılı),
**Öneri** (karar senin). Kaynak yollar `server-src/src/` altındadır.

## 1. Sonuç (önce bunu oku)
1. **Bugün de var olan, motordan bağımsız bir veri kaybı yolu var (Kanıtlı, gözlenmedi).** `db` oyuncu/item/quest
   kaydını kuyruğa atar atmaz "kaydedildi" sayıyor; `AsyncSQL` çoğu hatada sorguyu atıyor; birkaç hatada ise kuyruğu
   **kilitliyor**. Bunların düzeltilmesi InnoDB'den önce gelmeli: InnoDB bu yolu daha sık tetikleyebilir (satır kilidi).
2. **Uygulama tablolarının tamamı InnoDB'ye çevrilebilir (Kanıtlı).** account/common/player: 33/33; log: 15/15 (11'i
   `ROW_FORMAT=DYNAMIC` ile). Veri satır satır aynı kaldı. **Aria/MyISAM'da kalması gereken tablo bulunmadı.** Tek kod
   engeli: `game/log.cpp`'deki `INSERT DELAYED` InnoDB'de hata veriyor (ERROR 1616) → aynı sürümde kaldırılmalı.
3. **Performans etkisi bilinmiyor.** "InnoDB daha hızlı/yavaş" iddia edilmiyor. Savaş kodu DB'ye dokunmuyor (Kanıtlı);
   en çok yazılan tablolar (item, quest, affect) zaten InnoDB. Kabul kriteri: aynı veri, aynı VM, aynı yükle önce/sonra
   ölçümde regresyon yok. Bunun için önce ölçüm altyapısı gerekiyor.
4. **Sıra revize edildi:** ölçüm altyapısı → AsyncSQL/kayıt güvenilirliği düzeltmesi → şema yönetimi (sürümlü migration
   + motor kuralı) → dönüşüm (ilk migration) → yedeği kilitsiz moda geçirme → yük testiyle kabul. Ayrıntı: bölüm 7.

## 2. AsyncSQL: mevcut davranış (Kanıtlı, `libsql/AsyncSQL.cpp`)
Üç yol var; `db`'de her slot için üç ayrı bağlantı açılıyor (`db/DBManager.cpp:115-153`):

| Yol | Bağlantı | Hata olursa |
|---|---|---|
| `AsyncQuery` | `m_asyncSQL` worker | log + kuyruktan atılır; çağırana hiçbir şey dönmez |
| `ReturnQuery` | `m_mainSQL` worker | log + atılır; sonuç `uiSQLErrno` ile döner, çağıran bakarsa görür |
| `DirectQuery` | `m_directSQL`, çağıranın thread'inde, **retry yok** | log + `uiSQLErrno`; çoğu çağıran bakmıyor (ör. `game/char_change_empire.cpp:169`) |

**`ChildLoop` retry akışı gerçekte nasıl işliyor (`AsyncSQL.cpp:370-471`):**
- Worker `m_queue_query` doluysa uyanır (`:378-380`), `CopyQuery` hepsini `m_queue_query_copy`'ye taşır ve `count` =
  kopya kuyruğunun boyu (`:386`, `:274-288`).
- Retry listesindeki bir hatada sorgu **kuyruktan alınmadan** `continue` edilir (`:446-448`) ama `while (count--)` sayacı
  yine azalır. Tek sorguluk partide `count` 1 → ilk deneme başarısız → döngü biter → **hiç tekrar denenmez.**
- Sorgu kopya kuyruğunun başında kalır; worker tekrar `m_queue_query`'yi bekler (`:378`). Kopya kuyruğuna bakmadığı için
  **yeni bir sorgu gelene kadar uyur.** Arkadaki bütün sorgular da onun arkasında bekler (kuyruk başı tıkanması).
- Retry listesinde **kalıcı** hatalar var: `ER_INVALID_USE_OF_NULL`, `ER_HOST_NOT_PRIVILEGED`, şifre hataları,
  Aria/MyISAM'ın `ER_CRASHED_ON_USAGE`/`ER_NOT_KEYFILE`'ı (onarılana kadar). Böyle bir hatada her yeni partide aynı sorgu
  `count` kez × 100 ms denenir, **o bağlantının bütün yazmaları süresiz durur.**
- Kapanış: `DBManager.Quit()` worker'ları `join` eder (`db/Main.cpp:94`, `db/DBManager.cpp:44-56`). Worker o partiyi
  bitirir, sonra sadece `m_queue_query`'yi boşaltır (`AsyncSQL.cpp:474-530`); **kopya kuyruğunda kalan atılır.** Bekleyen
  sayısı da sadece `m_queue_query`'yi sayar (`db/DBManager.h:52-57`, `db/Main.cpp:97-108`).
- `:383` `m_queue_query.empty()` kilitsiz okunuyor (veri yarışı; etkisi küçük ama tanımsız davranış).
- Bağlantı: `CLIENT_MULTI_STATEMENTS` açık (`:77`), `MYSQL_OPT_RECONNECT` açık (`:83-84`). 2006 (gone away) retry
  listesinde; **2013 (sorgu sırasında bağlantı koptu) değil** → atılır.

**Birden çok ifade (`CLIENT_MULTI_STATEMENTS`):** kodda tek sorguda `;` ile birden çok ifade gönderen yer **yok** (arama:
`game`, `db`). `SQLMsg::Store()` sonraki ifadelerin sonuçlarını boşaltıyor ama 2.+ ifadedeki hatayı **kaydetmiyor**
(`AsyncSQL.h:106-127`). `AsyncQuery` yolunda `Store()` hiç çağrılmıyor: gelecekte eklenecek çok ifadeli bir async sorgu
bekleyen sonuç bırakır, sonraki sorgu "Commands out of sync" (2014) alıp atılır. Bugün kısmi tekrar (çift işlem) riski yok;
bayrak gereksiz ve SQL enjeksiyonunu büyütür → **Öneri: kapat** (stored procedure de kullanılmıyor).

## 3. "Sessiz veri kaybı" çağrı yolu (Kanıtlı kod yolu; test kullanımında gözlenmedi: syserr'de 0 SQL hatası)
Örnek: oyuncu A, item X'i B'ye verir.
1. `game` → `db`: item cache güncellenir; `cache::Put` `m_bNeedQuery = true` (`common/cache.h:24-30`).
2. ≤5 dk sonra `cache::Flush` → `CItemCache::OnFlush` → `ReturnQuery("REPLACE INTO item ... owner=B", QID_ITEM_SAVE)`
   (`db/Cache.cpp:138-143`) ve **hemen** `m_bNeedQuery = false` (`common/cache.h:47-55`). Veritabanından onay beklenmez.
3. Sorgu başarısız olur. Gerçekçi tetikleyiciler: MariaDB yeniden başlatma/çökme/OOM sırasında 2013 (retry yok, atılır);
   InnoDB'de kilitlenme 1213 ya da kilit zaman aşımı 1205 (retry yok, atılır); retry listesindeki kalıcı bir hata (kuyruk
   tıkanır, kapanışta atılır).
4. `db` sonucu yok sayar: `QID_ITEM_SAVE`, `QID_PLAYER_SAVE`, `QID_QUEST_SAVE` için sadece `break`
   (`db/ClientManager.cpp:2557-2562`).
5. Item tekrar değişmezse önbellek bir daha yazmaz; çıkışta önbellek silinir. Veritabanında X'in sahibi hâlâ A.
   Sonraki girişte X A'da görünür, B'de yoktur (eski DB durumuna dönme / değer kaybı). **Item çoğalması (dupe)
   kanıtlanmadı:** önbellek item kimliğiyle tek kayıt tuttuğu için kod okumama göre X iki yerde olmaz. Doğrulama:
   `docs/engineering/db-step1-measurement.md` → ticaret testi.

`item`'a `db` içinden üç bağlantı yazıyor: `REPLACE` ana bağlantı (`db/Cache.cpp:143`), `INSERT` senkron
(`db/ClientManager.cpp:748`), `DELETE` async (`:1546`). InnoDB satır kilitleri bu bağlantılar arasında çakışabilir.
Kilitlenme olasılığı **Bilinmiyor** (yük testinde `Innodb_deadlocks` ile ölçülecek). `item`/`quest` bugün zaten InnoDB.

## 4. Motor envanteri ve dönüşüm denemesi (Kanıtlı)
- account/common/player: 33 Aria → InnoDB: 2,3 sn, 0 uyarı, `CHECKSUM TABLE` ve satır sayıları 33/33 aynı.
- log: 8 Aria (`transactional=0`) + 7 MyISAM. 11'i `ROW_FORMAT=FIXED` → düz `ALTER` "Wrong create options" (errno 140);
  `ENGINE=InnoDB ROW_FORMAT=DYNAMIC` ile 15/15 çevrildi. 3 tabloda (`bootlog`, `levellog`, `money_log`) `CHECKSUM TABLE`
  farklı çıktı ama bütün sütunların hex içeriği sıralı olarak **birebir aynı** → `CHECKSUM TABLE` satır biçimine bağlı;
  motorlar arası doğrulamada kullanılamaz.
- Kod bağımlılıkları: `INSERT DELAYED` (`game/log.cpp:53, 80, 94, 106, 171, 177, 191, 200`) InnoDB'de **ERROR 1616**;
  `DELAYED`'sız aynı sorgular InnoDB'de çalıştı. Oyun bu sorguları zaten ayrı thread'den gönderiyor (`game/log.cpp:41`),
  `DELAYED` bir şey kazandırmıyor. Transaction yok, `LOCK TABLES` yok, FULLTEXT/SPATIAL indeks yok (sadece BTREE),
  çalışma anında tablo oluşturan tek yer devre dışı hotbackup (`db/HB.cpp:76`, `g_bHotBackup=false`).
- **Sonuç: Aria/MyISAM'da kalması gereken uygulama tablosu bulunmadı.** MariaDB'nin `mysql.*` tabloları kapsam dışı.
- Yan bulgu: `hotbackup` şeması kullanılmıyor; ayrı temizlik konusu.

## 5. Performans: ne biliniyor, ne bilinmiyor
**Kanıtlı:**
- Hasar/beceri/durum/hareket hesabı DB'ye dokunmuyor: `game/battle.cpp`, `char_state.cpp`, `char_resist.cpp`,
  `sectree_manager.cpp` → 0 DB çağrısı.
- Savaşın DB'ye değdiği yerler olay bazlı ve async: ölüm/öldürme/düşen item log'ları (`game/char_battle.cpp:1199-1491`,
  `char_skill.cpp`), yang log'u (`char_battle.cpp:732`), affect paketleri `db`'ye (`char_affect.cpp`).
- Kalabalık savaşta DB yükü ağırlıkla log ekleme ve `db` önbellek yazmaları. Oyun döngüsü bunları beklemez, ama MariaDB
  aynı makinede: CPU/disk rekabeti oyun döngüsünü dolaylı etkileyebilir.
- InnoDB ayarları varsayılan: buffer pool 128 MB, `innodb_flush_log_at_trx_commit=1`, redo 96 MB. Ölçmeden değişmeyecek.

**Bilinmiyor (ölçülecek):** dönüşüm öncesi/sonrası DB gecikmesi, kilit beklemeleri, kilitlenme sayısı, disk I/O,
`AsyncSQL` kuyruk derinliği, oyun döngüsüne dolaylı etki, büyük veri ve yüksek oyuncu sayısındaki davranış.

## 6. DB sözleşmesi (öneri)
Her madde mevcut repoya ve ölçüme dayanıyor.

| # | Kural | Gerekçe | Nasıl zorlanır |
|---|---|---|---|
| D-1 | Uygulama tabloları **InnoDB** | 4. bölüm; kilitsiz tutarlı yedek; çökme güvenliği; transaction imkânı | Katmanlı, production'a varmadan hata: (1) repo kontrolü: şema/migration dosyasında InnoDB dışı `ENGINE=` → kontrol script'i/CI hata verir; (2) migration uygulayıcısı her migration'dan sonra uygulama şemalarında InnoDB dışı tablo sayar, varsa durur; (3) MariaDB `enforce_storage_engine=InnoDB` + `NO_ENGINE_SUBSTITUTION` (bugün VM'de açık: `docs/build-and-run.md` → Veritabanı). Denendi: `ENGINE=Aria/MyISAM` ile `CREATE`/`ALTER` → ERROR 1290 (bu hatayı `NO_ENGINE_SUBSTITUTION` veriyor; o olmadan motor sessizce değişirdi); motorsuz `CREATE` → InnoDB; `mariadb-upgrade` rc=0; iç geçici tablolar etkilenmiyor; açıkça MEMORY/Aria geçici tablo da engelleniyor (kodda kullanım yok); (4) yedek/sağlık kaydı InnoDB dışı uygulama tablosu sayısını raporlar |
| D-2 | **Yeni** tablolarda primary key | InnoDB kümelenmiş indeks; satır bazlı doğrulama. Mevcut PK'sız tablolar (ör. bazı log tabloları) dönüşümde **körlemesine PK eklenmez**: büyük log tablolarında veri, indeks ve yazma büyütmesi ayrı ölçülür ve ayrı karar | Migration incelemesi + kontrol script'i (`information_schema`) yeni tabloda PK yoksa hata |
| D-3 | Charset/collation tabloda **açıkça** yazılır; **motor dönüşümünde değiştirilmez** | Bugün karışık: latin1 çoğunlukta, tek tek `euckr`, `gb2312`, `big5`, `ascii`, `utf8mb3` sütunlar (`information_schema.columns`). Client'tan gelen metin ham bayt olarak saklanıyor olabilir (**Bilinmiyor**: istemci kodlaması incelenmedi). Dönüşümde bayt/karşılaştırma davranışı korunur (denemede veri hex olarak birebir aynı) | Charset/collation modernizasyonu ayrı etki analizi ve ayrı karar; o zamana kadar yeni tablo mevcut kurala uyar ve sebebi yazılır |
| D-4 | Şema değişikliği sadece **sürümlü migration** ile | Bugün `server/sql/` tam döküm; geçmiş yok; production'da elle değişiklik izsiz | `server/sql/migrations/NNNN_ad.sql` + `schema_migrations` tablosu + uygulama script'i (bakım yedeğinden sonra). Taban şema = migration 0 |
| D-5 | Konumsal `INSERT ... VALUES` ve `SELECT *` yok, sütunlar adıyla | Sütun eklenince bunlar kırılır: `game/db.cpp:496`, `game/guild.cpp:87`, `game/log.cpp:106, 218, 250, 263, 300`, `game/messenger_manager.cpp:504`, `db/ClientManagerGuild.cpp:33`, `db/ClientManagerPlayer.cpp:1265, 1287, 1294`, `INSERT ... SELECT *` `db/ClientManagerPlayer.cpp:1086` (`player`→`player_deleted` sütunları birebir aynı olmak zorunda) | Mevcutlar düzeltilir; yeni kod incelemesinde kural |
| D-6 | Uyumluluk sırası "genişlet → binary → daralt" | Eski binary + yeni şema: yeni sütun varsayılanlı ve konumsal sorgu yoksa çalışır. Yeni binary + eski şema: yeni sütuna dokunan sorgu hata verir → önce migration, sonra binary; silme en son | Migration dosyasında "hangi binary'den itibaren gerekli" notu; geri dönüş = ters migration ya da bakım yedeği |
| D-7 | Her yazma için **retry sınıfı** | Bugün göreli `UPDATE` yok (`x = x + …` aramada çıkmadı); `UPDATE`/`REPLACE`/`DELETE` anahtarla ve mutlak → tekrar güvenli. Tehlikeli olan otomatik kimlikle yeni satır ekleyen `INSERT`ler (`game/guild.cpp:78`, `db/ClientManager.cpp:1759`, `db/ClientManagerPlayer.cpp:894`) ve log'lar. 1213/1205'te ifade geri alınır → her ifade için tekrar güvenli; 2013'te ifadenin uygulanıp uygulanmadığı belirsiz → sadece güvenli sınıf tekrar edilir | AsyncSQL'e sınıf bilgisi (ör. `idempotent`) ve sınıfa göre politika |
| D-8 | Ekonomi kayıtları atomik | Bugün oyuncu satırı (7 dk) ve item'lar (5 dk) ayrı zamanlarda, transaction'sız yazılıyor (`db/Main.cpp:29-30`). Bir craft'ın parçaları `db` çökerse yarım kalabilir (motordan bağımsız). InnoDB transaction'ı mümkün kılar ama kendiliğinden çözmez | Hedef mimari (ayrı iş): bir oyuncunun kirli satırları (player + item + quest + affect) tek transaction'da; çok oyunculu işlemler (ticaret, pazar, craft) bitince ilgili oyuncuların hemen ve birlikte yazılması. Her yeni ekonomi özelliğinin etki analizinde "yarıda kesilirse / iki kez çalışırsa" sorusu |
| D-9 | Yedek/geri yükleme kapsamı otomatik | Yedek şema bazlı döküyor → yeni tablolar otomatik dahil; tablo listesi `information_schema`'dan → varlık kontrolü otomatik. Sayım/checksum listesi bugün elle (`COUNT_TABLES`) | InnoDB sonrası anlık görüntüde kilit yok → bütün tablolar için içerik özeti (sıralı satır hash'i; `CHECKSUM TABLE` motorlar arası güvenilmez, bölüm 4) + bütünlük sorguları (sahipsiz item, aynı pozisyonda iki item, quest'i olmayan oyuncu vb.) |
| D-10 | Log/audit tabloları büyüme planlı | log en hızlı büyüyen şema; KVKK (A-2) | InnoDB + tarih bazlı bölümleme ya da arşivleme; saklama süresi kararı |

## 7. Revize sıra (her adım ayrı etki analizi + onay)
1. **Ölçüm altyapısı (davranış değişmez):** MariaDB sayaçlarını sağlık kaydına ekle (`Innodb_row_lock_waits`,
   `Innodb_row_lock_time_max`, kilitlenme sayısı, `Table_locks_waited`, yavaş sorgular, `Threads_running`, disk I/O);
   `AsyncSQL` kuyruk derinliği ve hata sayaçları. Bugünkü (Aria/MyISAM) taban çizgi ölçümü.
2. **AsyncSQL ve kayıt güvenilirliği (bug düzeltmesi, motordan bağımsız):** kuyruk başı tıkanmasız ve sınırlı retry;
   kalıcı hatalar listeden çıkar (kaydedilir + sayılır, kuyruğu kilitlemez); 1213/1205 güvenle tekrar; 2013 sadece güvenli
   sınıf; kapanışta kopya kuyruğu da boşaltılır ve sayılır; `CLIENT_MULTI_STATEMENTS` kapanır; `db`'de başarısız
   `*_SAVE` önbelleği tekrar kirli işaretler (bir sonraki flush yeniden dener). Hata enjeksiyonuyla test (bölüm 8).
   **Devreye almadan önce:** roadmap T-1 (`syserr.log` yeniden başlatmada korunur) ve T-2 / 1.9 (çalışan binary'nin
   sürümü kesin bilinir); önce/sonra karşılaştırmasında hata kanıtı ve ölçülen binary kanıtlanabilsin.
3. **Şema yönetimi:** migration dizini + `schema_migrations` + uygulama script'i + motor kuralı (D-1), D-5 düzeltmeleri.
   Uygulanmış son migration (şema sürümü) **makinece okunur** olmalı: salt-okuma okuyucu (yönetim servisi, teşhis yapan
   agent) "hangi şema çalışıyor" sorusunu veri okumadan cevaplayabilmeli (`docs/architecture.md` → "Kontrol katmanı ilkeleri").
4. **Dönüşüm = migration 0001:** bütün uygulama tabloları InnoDB (`ROW_FORMAT=DYNAMIC`, Aria seçenekleri temizlenir) +
   aynı sürümde `game/log.cpp`'den `DELAYED` kaldırılır. Bakım yedeğiyle geri dönüş.
5. **Yedek kilitsiz moda:** anlık görüntü; manifest bütün tablolar için içerik özeti + bütünlük sorguları.
6. **Yük testiyle kabul (roadmap 2.2):** aynı yük, önce/sonra; production kapısı.
7. Sonra (ayrı tasarım): oyuncu bazlı atomik kayıt (D-8).

**Not:** 2. adım, test VM'de bugünkü şemayla tek başına değerlidir. 4. adımın kabulü 1 ve 6'ya bağlıdır.

## 8. Test planı
**Doğruluk (2. adım):**
- Hata enjeksiyonu geçici MariaDB'de, gerçek `db`/`game` ile: (a) kilit tutan bir oturumla 1205 üret; (b) iki oturumla
  1213 üret; (c) sorgu sırasında MariaDB'yi yeniden başlat (2013); (d) kalıcı hata (izin kaldırma). Beklenen: (a)(b)
  tekrar edilip tek kez uygulanır; (c) sadece güvenli sınıf tekrar edilir; (d) kaydedilir, sayılır, kuyruk ilerler.
- Kayıt kaybı testi: item ver → kaydı hata ile başarısız yap → önbellek kirli kalmalı → sonraki flush'ta veritabanında doğru.
- Kapanış: kopya kuyruğunda bekleyen varken `service m2dev stop` → hepsi yazılmış olmalı.
- Çift işlem testi: log ve otomatik kimlikli `INSERT`'ler belirsiz hatada iki kez yazılmamalı.

**Dönüşüm (4. adım):** geçici örnekte migration → satır içerik özeti önce/sonra; VM'de bakım yedeği → migration → oyun
içi regresyon: giriş/çıkış, karakter oluşturma/silme, isim/imparatorluk değişimi, lonca kurma/yorum/üye, depo, ticaret,
mağaza, item düşür/al/yükselt, quest, evlilik, at adı, ışınlanma; syserr ve sağlık kaydı.

**Performans (1 ve 6):** aynı veri seti (gerçek + büyütülmüş), aynı VM, aynı yük; Aria/MyISAM ve InnoDB.
- Sınıf A — saf oyun yükü: yüzlerce bot, aynı harita, hareket, PvP/PvE, skill, mob güncellemesi, yoğun ağ.
- Sınıf B — A + DB yükü: item düşür/al, yang, quest, lonca, mağaza/pazar/depo/ticaret, giriş/çıkış, log, önbellek
  yazmaları, aynı anda sıcak yedek.
- Sinyaller: `late_pulses`, `iter_gap_max_us`, `work_max_us`, bölüm süreleri; DB sorgu gecikmesi; kilit bekleme
  süresi/sayısı; kilitlenme ve zaman aşımı sayısı; `AsyncSQL` kuyruk derinliği; CPU, RAM, disk I/O; yedek anında
  oyuncu etkisi; büyük veride davranış.
- Ayrım: InnoDB sonrası bir iyileşme görülürse bölüm sürelerinden (savaş `chr_us`/`event_us` mi, `io_us`/DB beklemesi mi)
  kaynağı ayrılır.
- Yük testi aracı (roadmap 2.2) yokken ara yöntem: oyun sırasında genel sorgu log'uyla gerçek sorgu karışımı yakalanır
  (ayrı onay; log hassas), iki motor üzerinde çok bağlantıyla yeniden oynatılır → DB tarafı karşılaştırması. Oyun
  döngüsü etkisi ve savaş senaryoları için gerçek yük testi şart.
- Kabul: Sınıf A ve B'de anlamlı regresyon yok; varsa sebep ölçümle bulunur. MariaDB ayarı sadece ölçüm gerektirirse ve
  tek tek değiştirilir.

## Kaynaklar
- MariaDB `innodb_rollback_on_timeout`: https://mariadb.com/docs/server/ref/mdb/system-variables/innodb_rollback_on_timeout
- InnoDB hata işleme (kilitlenme → transaction geri alınır, zaman aşımı → ifade geri alınır):
  https://dev.mysql.com/doc/refman/8.0/en/innodb-error-handling.html
- Aria + BACKUP STAGE: https://jira.mariadb.org/browse/MDEV-18573
