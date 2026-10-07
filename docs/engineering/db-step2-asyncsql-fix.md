# DB adım 2: AsyncSQL güvenilirlik düzeltmesi — kök neden ve etki analizi (revizyon 4)

**Durum: 2a kodlandı ve test VM'de geçici MariaDB ile test edildi (2026-10-07, dal `feat/asyncsql-2a`), kabul ve VM kurulumu bekliyor; sonuçlar `docs/worklog/2026-10-07-asyncsql-2a.md`. 2b ayrı onaya bağlı.** Şema ve canlı veri değişmedi. Bu revizyon kullanıcı
incelemesine göre yeniden yazıldı: hata kodları Connector kaynağından tek tek çıkarıldı, 1205/1213 gerçek sorgu aileleri ve
gerçek motorlarla test edildi, kısmi paket / H-1 / K7 için reproducer'lar eklendi. Revizyon 1'in iddiaları **yanlıştı ve
geri çekildi** (bölüm 0). **Revizyon 3:** sınıflandırma hata koduna değil **başarısızlık evresine** dayanıyor (bölüm 1); 2a
kapsamı kullanıcının kabul şartlarıyla yeniden yazıldı (bölüm 10). **Revizyon 4:** teslim/yürütme sonucu ile tekrar politikası
iki ayrı eksen (bölüm 1b; C1–C4 probları); mevcut ham SQL loglarının hassas veri denetimi (bölüm 7b). Etiketler: **Kanıtlı** (kod `yol:satır`, test ya da komut çıktısı), **Kanıt yok** (ne kontrol edildiği
yazılı), **Öneri** (karar senin). Kaynak yollar `server-src/src/` altında.

**Kanıt kapsamı (bütün sonuçlar bununla sınırlı):** vendored MariaDB Connector/C **3.4.5**, MariaDB Server **11.8.9**, TCP,
TLS ve sıkıştırma yok, `CLIENT_MULTI_STATEMENTS` açık (`libsql/AsyncSQL.cpp:196`), `MYSQL_OPT_RECONNECT` açık (`:202-206`);
canlı test VM'de (salt okuma, `information_schema` 2026-10-07): `autocommit=1`, `innodb_rollback_on_timeout=0`,
`innodb_lock_wait_timeout=50`, `lock_wait_timeout=86400`, trigger/prosedür/event **0**. Bunlardan biri değişirse (sürüm
yükseltme, TLS, trigger, transaction) sonuçlar geçersizdir; prob paketi yeniden koşulmalı.

**Araçlar:** `tools/sql-reliability/sqlrt.cpp` + `run.sh` (S1–S12, değişmez baseline) ve yeni
`tools/sql-reliability/sqlprobe.cpp` + `probe.sh` (bu revizyonun probları; gerçek `item`/`quest`/`player`/`guild_member`/
`account` DDL'leri `server/sql/*.sql`'den, motorlar canlıyla aynı). Geçici MariaDB, fail-closed koruma (geçici datadir +
rastgele token). Son koşular: 26 senaryo, 3'er tekrar, koruma öz-testi 3/3 ret; K7a/K7b dışında hepsi 3/3 birebir (K7a/b'de sadece ölçülen sayılar
değişiyor, davranış aynı).

## 0. Revizyon 1'den geri çekilenler
1. **"2006/2002/2003 sorgunun sunucuya hiç ulaşmadığını gösterir"** — hata koduyla bu kadar geniş söylenemezdi. Doğrusu bölüm 1:
   2003 bu Connector'da hiç dönmüyor; 2002/2006 için kod yolu + sunucu testi gerekti ve sonunda sınıf evreye bağlandı (madde 6).
2. **"1205/1213 her ifade için güvenle tekrar edilir"** — genel MariaDB davranışı uygulama sorgularıyla karıştırılmıştı. Doğrusu
   bölüm 2: sadece test edilen aileler, belirtilen koşullarda.
3. **"Göreli `UPDATE` (`x = x + …`) yok"** — **yanlıştı.** `db/ClientManager.cpp:3587-3589` hesap bakiyesini göreli güncelliyor
   (`` `cash` = `cash` + %d ``); arama deseni ters tırnaklı sütunları kaçırmıştı. Grep tabanlı envanter eksik kalabilir (bölüm 13).
4. "Tam bir kez (exactly-once)" dili kaldırıldı; aile başına semantik kullanılır (bölüm 3).
5. "Sınırsız kuyruk + uyarı", "≤ 20 sn kapanış", "ham SQL dead-letter" ve "sahipsiz item DELETE'ini main'e al" önerileri geri
   çekildi (bölüm 5, 6, 7, 8).
6. **(Revizyon 2'den)** "2002/2006 = gönderilmedi" sınıfı **hata koduna göre** kurulmuştu. Geri çekildi: hata kodu tek başına
   evreyi söylemiyor; sınıflandırma başarısızlık evresine dayanıyor (bölüm 1).
7. **(Revizyon 4'ten, 2a kodlaması sırasında bulundu)** "`mysql_real_query` gönderimde `-1`, okumada `1` döndürür; evre dönüş
   değerinden ayırt edilir" **yanlıştı.** Okuma fonksiyonu `mthd_my_read_query_result` de birkaç yolda `-1` döndürür
   (`libmariadb/mariadb_lib.c:2918` LOCAL INFILE, `:2940, 2943` sonuç kümesi metadata'sı, `:2951, 2956` EOF paketi);
   `mysql_real_query` bunu olduğu gibi geçirir (`:3007-3008`). Yani `rc=-1` okuma evresinde de görülebilir. **Düzeltme (2a):**
   AsyncSQL evreyi dönüş değerinden çıkarmaz; iki ayrı çağrı yapar: `mysql_send_query` (`:2582-2585`, yalnız
   `ma_simple_command`, `skip_check=1`) başarısızsa **gönderim**, `mysql_read_query_result` (`:2987-2990`) başarısızsa **okuma**
   (`libsql/AsyncSQL.cpp`, `CAsyncSQL::Attempt`). Bölüm 1'in gönderim/okuma sonuçları (P1–P5, C1–C4) bu iki çağrıyla aynı kod
   yollarını kapsar; aşağıdaki tablolarda `rc=-1` = "gönderim evresi başarısız", `rc=1` = "okuma evresi başarısız" diye okunmalı.

## 1. Başarısızlık evresi ve hata kodları (Connector 3.4.5 kaynağı + sunucu testleri)
**Evre, çağrıdan ayırt ediliyor (Kanıtlı kod; bölüm 0 madde 7 ile düzeltildi):** `mysql_real_query`
(`libmariadb/mariadb_lib.c:2996-3010`) iki evreyi sırayla çalıştırır: gönderim (`ma_simple_command` → `mthd_my_send_cmd`,
`:3005-3006`, hata → `-1`) ve cevap okuma (`db_read_query_result` → `mthd_my_read_query_result` → `ma_net_safe_read`,
`:3007-3008, 2889-2903, 207-231`). Okuma evresi de bazı yollarda `-1` döndürdüğü için (`:2918, 2940, 2943, 2951, 2956`) **dönüş
değeri evreyi kesin söylemez.** Kesin ayrım iki ayrı çağrıyla yapılır: `mysql_send_query` (`:2582-2585`, yalnız gönderim) ve
`mysql_read_query_result` (`:2987-2990`, yalnız okuma). Bu bölümde `rc=-1` / `rc=1` = gönderim / okuma evresi başarısız.
Yollar `server-src/vendor/mariadb-connector-c-3.4.5/` altında. **2a öncesi AsyncSQL evreyi hiç kullanmıyordu**
(`if (mysql_real_query(…))`, revizyon 4 anındaki `libsql/AsyncSQL.cpp:295, 574, 672`); 2a iki ayrı çağrıya geçti.

**Gönderim evresi (`rc = -1`) — "istek tamamı teslim edilmeden döndü" (Kanıtlı):**
- `mthd_my_send_cmd` (`mariadb_lib.c:401-489`) yalnız şu yollarda hata döner: (a) gönderimden önce soket ölü + yeniden bağlanma
  başarısız (`:409-421`, hiçbir bayt yazılmadı); (b) bekleyen sonuç var → 2014 (`:422-427`, yazılmadı); (c) yazma başarısız →
  yeniden bağlanma başarısız ya da ikinci yazma başarısız (`:461-476`).
- Yazma döngüsü (`libmariadb/ma_net.c` `ma_net_real_write`) bir yazma çağrısı başarısız olunca **`pos != end` iken** `1` döner;
  yani hata dönen her durumda isteğin son baytı kernel'e bile verilmemiştir. Yazma tamamlanırsa gönderim evresi `0` döner ve
  `mysql_real_query` okuma evresine geçer (`skip_check=1`); gönderim tamamlandıktan sonra gönderim evresinden hata dönen yol yok.
- Sunucu eksik paketi **çalıştırmıyor**: üç kesilme noktası, 3/3: yükün yarısı (P1), başlığın 2 baytı (P1b), son bayt hariç her
  şey (P1c) → uygulanmadı. Kontrol (P0): tam paket → uygulandı.
- Uçtan uca: P3 sunucu kapalı → `rc=-1`, 2002, uygulanmadı; G1 aynı durumda AsyncSQL tekrarıyla sunucu dönünce bir kez uygulandı.
- **Sonuç:** `rc = -1` → **teslim edilmedi → uygulanmadı** (hata kodu ne olursa olsun). Bu, tekrarın **zararsız** olduğunu
  söyler, **yapılacağını değil**: 1045/1049/1040/1226 gibi yapılandırma/kaynak hataları da bu evrede döner (C1–C4); tekrar
  kararı ikinci eksende (bölüm 1b).

**Okuma evresi (`rc = 1`) — istek tamamı yazıldı:**
- Bağlantı hatası (2013 vb.) → **belirsiz:** P2 (tam paket, cevap okunmadan kopma) → **uygulandı**; P4 (yürütmede bağlantı
  öldürüldü) ve P5 (yürütmede sunucu kapandı) → `rc=1`, 2013, uygulanmadı. Aynı evre + aynı kod, iki sonuç.
- Sunucu hata paketi (1205, 1213, sözdizimi…) → sonucu ailenin kanıtına bağlı (bölüm 2).

**Yalnız hata kodu biliniyorsa (evre bilinmiyor):** **her bağlantı hatası belirsiz** sayılır — 2002 ve 2006 dahil.

| Kod | Nerede üretiliyor (Kanıtlı kod) | Evre | Sınıf |
|---|---|---|---|
| 2002 `CR_CONNECTION_ERROR` | soket bağlanamadı (`plugins/pvio/pvio_socket.c:831, 986`); sorguya yeniden bağlanma başarısızlığından kopyalanır (`mariadb_lib.c:2186-2199`) | gönderim (`-1`) | `rc=-1` ile: teslim edilmedi; yalnız kodla: belirsiz |
| 2003 `CR_CONN_HOST_ERROR` | **dönmüyor**: "never sent to a client" (`include/errmsg.h:45`); soket katmanı 2003 metniyle 2002 kullanıyor (`pvio_socket.c:986-987`) | — | — |
| 2006 `CR_SERVER_GONE_ERROR` | ikinci yazma başarısız (`mariadb_lib.c:472-476`); yeniden bağlanmaya izin yok (`:2158-2165`). Okuma evresinde üretilmiyor | gönderim (`-1`) | `rc=-1` ile: teslim edilmedi; yalnız kodla: belirsiz |
| 2013 `CR_SERVER_LOST` | el sıkışma (`:1880-1886, 2058, 2112-2115`) **ve** cevap okuma (`:217-231`) | her ikisi | `rc=-1` ile: teslim edilmedi; `rc=1` ile: **belirsiz** |
| 1158–1161 `ER_NET_*` | sunucu hata paketi olarak gelebilir (`:233-262`); gözlenmedi | okuma | belirsiz (1160/1161 yürütmeden sonra olabilir) |
| 1927 / 1053 | sunucu hata paketi; gözlenmedi (P4/P5'te istemci 2013 gördü) | okuma | belirsiz; Aria/MyISAM'da kısmi uygulama olabilir |
| 1205 / 1213 | sunucu hata paketi | okuma | test edilen ailelerde kanıtlı geri alma (bölüm 2) |
| 2014 | bekleyen sonuç (`:422-427`) | gönderim | teslim edilmedi; kendi hatamız (S11) |
| diğer sunucu hataları | sunucu hata paketi | okuma | kalıcı; InnoDB tek ifade uygulanmadı, Aria/MyISAM çok satırlı ifade kısmen uygulanmış olabilir |

**Sözleşme (Öneri):** hata kodu teslim/yürütme garantisi **vermez**. Tekrar kararının girdisi **(evre, kod, aile)**:
`rc = -1` → teslim edilmedi; `rc = 1` + bağlantı hatası → belirsiz; `rc = 1` + sunucu hata paketi → aile kanıtına göre; evre
bilinmiyorsa → belirsiz. Kanıt kapsamı: bu Connector sürümü (gönderim yolu ve yazma döngüsü) ve bu sunucu sürümü (P1/P1b/P1c).

## 1b. İki ayrı eksen: sonuç ve tekrar politikası (revizyon 4)
"Teslim edilmedi" **otomatik tekrar demek değil.** Her başarısızlık iki bağımsız eksende sınıflanır:
- **Sonuç (ne oldu):** `APPLIED` · `NOT_DELIVERED` · `ROLLED_BACK` · `AMBIGUOUS` · `PERMANENT` (sorgu hatası) · `UNEXECUTED_AT_QUIT`.
- **Tekrar politikası (ne yapılır):** `TRANSIENT_CONNECTION` · `RETRYABLE_AFTER_PROVEN_ROLLBACK` · `QUERY_PERMANENT` ·
  `CONNECTION_CONFIG_FATAL` · `RESOURCE_LIMIT` · `INTERNAL_PROTOCOL_STATE` · `AMBIGUOUS_NO_RETRY`.
  "Tekrar etmiyoruz" ile "kalıcı hata" aynı şey değil: `RESOURCE_LIMIT` geçici bir doygunluk olabilir; 2a onu tekrar etmez ama
  kalıcı da saymaz.

**Kanıt (`sqlprobe` C1–C4, 3/3):** yeniden bağlanma gereken bir ifadede bağlantı/yapılandırma hataları **gönderim evresinde**
(`rc=-1`) döner — ifade teslim edilmedi ama durum geçici değil. Bugünkü kod bunların hiçbirinde döngüye girmiyor (tek
başarısızlık satırı, `retrying` 0); yani **bugün de** kayıp var, döngü yok.
| Prob | Durum | `rc` | Kod |
|---|---|---|---|
| C1 | kullanıcının şifresi değişti (access denied) | -1 | 1045 |
| C2 | veritabanı silindi (unknown database) | -1 | 1049 |
| C3 | sunucu geneli bağlantı sınırı dolu (ayrıcalıksız kullanıcı; `max_connections` en az 10) | -1 | 1040 |
| C4 | kullanıcı başına bağlantı sınırı (`MAX_USER_CONNECTIONS`) | -1 | 1226 |
| P3 / G1 | sunucu kapalı | -1 | 2002 |
| S11 | bekleyen sonuç varken gönderim | -1 | 2014 |

**Eşleme (Öneri; 2a davranışı):**
| Evre + kod | Sonuç | Politika | 2a davranışı |
|---|---|---|---|
| `rc=-1` + 2002, 2006, 2013 (sunucuya ulaşılamıyor / bağlantı koptu) | NOT_DELIVERED | **TRANSIENT_CONNECTION** | yerinde tekrar, 100 ms aralık (bugünkü değer), `Quit` ile kesilir; `syserr`'e yalnız **durum değişiminde** (ilk başarısızlık, düzelme) bir satır; her deneme sayaçta |
| `rc=-1` + 1045, 1044, 1049, 1129, 1130, 2005, 2059 (kimlik, veritabanı, host, yapılandırma) | NOT_DELIVERED | **CONNECTION_CONFIG_FATAL** | **tekrar yok:** ileti başarısız (ledger + sayaç); `syserr`'e durum değişiminde bir kritik satır. Sonraki ileti tek bir bağlantı denemesi yapar, beklemez; 100 ms döngü yok. (Ne yapılacağı — tutmak, bakım moduna geçmek — ayrı karar; bu revizyon yeni politika icat etmez) |
| `rc=-1` + 1040, 1203, 1226 (sunucu/kullanıcı bağlantı ya da kaynak sınırı) | NOT_DELIVERED | **RESOURCE_LIMIT** (geçici doygunluk olabilir; kalıcı sayılmaz) | **otomatik tekrar şimdilik yok:** ileti başarısız (ledger + sayaç + durum değişiminde bir satır). Eşik, bekleme ya da degraded davranış eklenmez; ileride kaynak/degraded politikasına bırakılır |
| `rc=-1` + 2014, 2027, 2008, 2000 | NOT_DELIVERED | **INTERNAL_PROTOCOL_STATE** | tekrar yok; ileti başarısız + kritik satır; 2014'ün kaynağı (S11) 2a'da düzeltiliyor |
| `rc=-1` + 2020 (paket çok büyük) | NOT_DELIVERED | **QUERY_PERMANENT** | tekrar yok (ifadeye özgü) |
| `rc=-1` + **listede olmayan kod** | NOT_DELIVERED | **CONNECTION_CONFIG_FATAL** (varsayılan) | tekrar yok — bilinmeyen kod geçici sayılmaz |
| `rc=1` + 1205 / 1213 | `rollback_proven` ailede ROLLED_BACK; **işaretsiz ailede AMBIGUOUS** (kanıt yoksa belirsiz) | işaretli ailede RETRYABLE_AFTER_PROVEN_ROLLBACK; işaretsizde AMBIGUOUS_NO_RETRY | 2a'da hiçbir aile işaretli değil → tekrar yok, ledger'a AMBIGUOUS + hata kodu |
| `rc=1` + 2013, 1158–1161, 1927, 1053 | AMBIGUOUS | AMBIGUOUS_NO_RETRY (`idempotent` aile hariç) | tekrar yok; ledger'a AMBIGUOUS |
| `rc=1` + diğer sunucu hataları | PERMANENT | QUERY_PERMANENT | tekrar yok |
| Kapanışta işlenemeyen | UNEXECUTED_AT_QUIT | — | ledger + sayaç |

**Döngü olmadığının kanıtı (2a testi):** C1–C4 yeniden koşulur → her ileti için **tek** deneme, `syserr`'de durum değişimi başına
bir satır, worker bir sonraki iletiye geçer; 100 ms döngü yalnız TRANSIENT_CONNECTION'da (G1/P3) ve `Quit` ile kesilir.
Sınırlama: sunucu yeniden başlarken el sıkışmada 1053 (shutdown in progress) gibi başka kodlar dönebilir; **kanıt olana kadar
transient listesinde değiller** (varsayılan: tekrar yok); bu durum MariaDB yeniden başlatma testinde ölçülecek.

## 2. 1205 / 1213: gerçek sorgu aileleri ve motorlar (Kanıtlı, `sqlprobe` R1–R5, 3/3)
Koşullar: oturum `autocommit=1`, açık transaction yok, tek ifade, trigger yok, çok ifade yok (canlı ve probe aynı).

| Prob | Aile (gerçek biçim) | Motor | Hata | Hata sonrası | Aynı ifade tekrar |
|---|---|---|---|---|---|
| R1 | item kaydı `REPLACE INTO item (…) VALUES(…)` (`db/Cache.cpp:138-143`) | InnoDB | 1205 (satır kilidi) | `count` değişmedi, satır sayısı 1 (REPLACE'in sil+ekle'si de geri alındı) | uygulandı, satır 1 |
| R2 | oyuncu silme `DELETE FROM item WHERE owner_id=… AND (window…)` (`db/ClientManagerPlayer.cpp:1153`), kilitlenme kurbanı | InnoDB | 1213 (`Innodb_deadlocks=1`) | iki satır da duruyor (ifade tümden geri alındı) | ikisi de silindi |
| R3 | `REPLACE INTO quest … SELECT … FROM guild_member` (`db/ClientManagerGuild.cpp:112`), hedef satırlardan biri kilitli | InnoDB ← Aria (okuma) | 1205 | yeni değerli satır **0** (kilitsiz satır da yazılmadı) | 2 satır yazıldı |
| R4 | oyuncu kaydı `UPDATE player SET … WHERE id` (Aria), başka oturum `LOCK TABLES player WRITE` | Aria | 1205 (metadata/tablo kilidi, `lock_wait_timeout=1` ile) | değişmedi | uygulandı |
| R5 | `ChargeCash` `` update account set `cash` = `cash` + n `` (`db/ClientManager.cpp:3587`) | InnoDB | 1205 | `cash` aynı (100) | **bir kez** uygulandı (150) |

**Sonuç ve sınırlar:**
- Test edilen 5 ailede 1205/1213 ifadenin etkisini tümden geri aldı → bu aileler için "**kanıtlı geri almadan sonra tekrar
  edilebilir**". Diğer aileler test edilene kadar bu sınıfa girmez.
- **1213 yalnız InnoDB'de.** Aria/MyISAM'da satır kilidi yok; 1213 oluşmaz.
- **Üretimdeki Aria davranışı farklı:** `lock_wait_timeout=86400` → Aria tablo/metadata kilidi çakışmasında 1205 pratikte **hiç
  gelmez**; worker 24 saate kadar **hatasız bekler**. InnoDB'de `innodb_lock_wait_timeout=50` → her deneme 50 sn'ye kadar
  bekletir. Tekrar politikası deneme süresini bu yüzden sayıyla değil süreyle de görmeli (bölüm 5).
- MyISAM log tabloları (`log`, `money_log`, `goldlog`…) test edilmedi.

## 3. Sorgu aileleri → güvenlik semantiği (Öneri; dayanak Kanıtlı)
Semantik sözlüğü: **at-most-once** (en çok bir kez; belirsizlikte tekrar yok, kayıp olabilir) · **retryable after proven
rollback** (geri alma kanıtlıysa tekrar) · **idempotent convergence** (tekrar aynı son duruma götürür) · **at-least-once**
(kayıp yok, çift olabilir) · **ambiguous outcome** (sonuç bilinmiyor; kayda geçer, otomatik karar yok).

| Aile | Tablo / motor | Yol / bağlantı | Biçim | Teslim edilmedi + TRANSIENT_CONNECTION | 1205/1213 | Belirsiz (`rc=1` bağlantı hatası, ya da evre bilinmiyor) |
|---|---|---|---|---|---|---|
| item kaydet | item / InnoDB | db main `ReturnQuery` | mutlak tam satır `REPLACE` | tekrar | tekrar (R1) | **idempotent convergence** (aynı FIFO'da yerinde) |
| item sil (id) | item / InnoDB | db main / async | `DELETE … WHERE id` | tekrar | test yok → tekrar yok | idempotent convergence |
| item sil (sahip) | item / InnoDB | db **direct** | çok satırlı `DELETE` | (direct: tekrar yok) | R2 kanıtlı, ama direct yolda tekrar yok | — |
| oyuncu kaydet | player / **Aria** | db main | mutlak `UPDATE … WHERE id` | tekrar | tekrar (R4; üretimde 86400 sn bekleme) | idempotent convergence |
| quest kaydet/sil | quest / InnoDB | db main | `REPLACE`/`DELETE` | tekrar | test yok → tekrar yok | idempotent convergence |
| lonca bırakma quest | quest / InnoDB ← guild_member / Aria | db async | `REPLACE … SELECT` | tekrar | tekrar (R3) | idempotent convergence |
| depo boyut/altın/şifre | safebox / Aria | db/game | mutlak `UPDATE` | tekrar | test yok | idempotent convergence (**şifre açık metin**, A-13) |
| **hesap bakiyesi** | account / InnoDB | db async `SQL_ACCOUNT` | **göreli** `cash = cash + n` | tekrar | tekrar (R5) | **at-most-once** (tekrar = çift yükleme) + kayıt + elle mutabakat |
| item_award ver | item_award / Aria | game async | `INSERT … WHERE NOT EXISTS (login, why)` | tekrar | test yok | idempotent convergence (`why` tekilse) |
| depoya ödül item'ı | item / InnoDB | db direct | açık id'li `INSERT` | (direct) | — | PK çakışması (1062) → at-most-once |
| oyun log'ları | log* / Aria, MyISAM, InnoDB | game LogManager async | otomatik id `INSERT` | tekrar | test yok → tekrar yok | **at-most-once** |
| loginlog2 çıkış | loginlog2 / InnoDB | game LogManager async | `SET @i = …` + `UPDATE … WHERE id=@i` (iki ifade, oturum değişkeni) | tekrar (ama yeniden bağlanmada `@i` kaybolur → `UPDATE` 0 satır) | — | at-most-once (best-effort log) |
| lonca seviye/puan | guild / InnoDB | game async | mutlak `UPDATE` | tekrar | test yok | idempotent convergence |
| yüklemeler (`SELECT`) | çeşitli | `ReturnQuery` | okuma | tekrar | tekrar | tekrar (yan etki yok) |

**Varsayılan (işaretlenmemiş aile):** sadece "teslim edilmedi + TRANSIENT_CONNECTION" tekrar edilir; yapılandırma/iç hatalar,
1205/1213 ve belirsizlikte tekrar yok,
kayda geçer. Bir aile ancak testle kanıtlandıktan sonra daha geniş sınıfa alınır. Hiçbir aile için **exactly-once**
denmez: belirsiz ağ hatasında uygulama düzeyinde tekil işlem anahtarı (ör. ödeme/yükleme kimliği) olmadan bu garanti yok.

## 4. Belirsizlik sonucu (2002 / 2006 / 2013)
- **Evre biliniyorsa:** `rc=-1` → teslim edilmedi (kod + P1/P1b/P1c + P3) → her aile için tekrar **zararsız**; tekrar edilip
  edilmeyeceği ikinci eksende (bölüm 1b: yalnız TRANSIENT_CONNECTION). `rc=1` + bağlantı
  hatası → belirsiz (P2 uygulandı; P4/P5 uygulanmadı) → sadece idempotent convergence ailelerinde tekrar.
- **Evre bilinmiyorsa (yalnız hata kodu):** 2002, 2006, 2013 ve bütün bağlantı hataları **belirsiz**; kör tekrar yok.
- **2a'nın şartı:** AsyncSQL başarısızlık evresini (`mysql_send_query` / `mysql_read_query_result`, bölüm 0 madde 7) saklar ve
  sınıflandırmaya katar; evresi bilinmeyen hiçbir
  başarısızlık güvenli sayılmaz.

## 5. Kuyruk: uzun DB kesintisi ve geri basınç (Öneri: seçenekler, sayı yok)
**Sorun:** "teslim edilmedi" (`rc=-1`) sınıfını yerinde tekrar etmek doğru (FIFO ve bariyer korunur, bölüm 8) ama DB uzun süre yoksa kuyruk
bellekte büyür; süreç belleği biterse çöküş, daha büyük kayıp demek. Sessiz atma da kabul edilmiyor.

**Bugün ölçülenler (test VM, en çok 1 oyuncu; 2.500 oyuncu için baseline DEĞİL):**
- Kuyruğa girme (`sql_*.log` `pushed`, 2 gün): db ort. 9,7 / en çok **7.089** ifade/10 sn (açılış/kapanış flush'ı); game
  çekirdekleri en çok 20/10 sn. Görülen en büyük `q+cq`: db 12.
- Ortalama ifade boyutu ≈ **333 B** (`Bytes_received / Questions` = 41,25 MB / 123.850; bütün bağlantılar, yaklaşık).
- VM: 6,1 GB RAM; anlık 699 MB boş + 4,5 GB inactive.
- Engelleme süresi kaynakları: InnoDB kilidi deneme başına ≤ 50 sn; Aria kilidi ≤ 86400 sn (hatasız).

**Seçenekler (birlikte kullanılabilir):**
| Seçenek | Ne yapar | Bedel / risk |
|---|---|---|
| Ölçüm: kuyruk derinliği + **bayt** + en eski yaş + takılma süresi | `q`, `cq`, `oldest_ms`, `stuck_ms` 1c'de var; **bayt yok** (eklenmeli) | ucuz; tek başına koruma değil |
| Yüksek su işareti (derinlik/bayt/yaş) → durum (normal/degraded/critical) | panele/uyarıya durum | eşikler ölçümle belirlenecek |
| Üretici geri basıncı (oyun thread'i bekler) | bellek sınırlı | **oyun döngüsü durur** → bütün oyuncular donar; önerilmez |
| Oyun düzeyinde kabul kontrolü (degraded): yeni giriş, ticaret, mağaza, item üretimi gibi durum değiştiren işlemleri geçici durdur | kuyruğa yeni ekonomi yazması girmez | game entegrasyonu gerekir (ayrı kapsam, yüksek risk) |
| Bakım modu / kontrollü kapanış (critical'de) | önbellek flush + kalanlar kayda, süreçler kapanır | oyuncular düşer; kayıp sınırlı ve **görünür** |
| Kritik eşikte fail-closed (ekonomi işlemleri reddedilir) | dupe/kayıp penceresini kapatır | oyuncu deneyimi; tasarım kararı |

**Eşik türetmek için ölçülecekler:** yük testinde (roadmap 2.2) aile başına ifade boyutu dağılımı ve üretim hızı, süreç başına
RSS ve boş RAM, ileti başına bellek (ifade + `SQLMsg` yükü), MariaDB yeniden başlatma/çökme süresi (test VM'de ölçülecek),
kesinti senaryoları (yeniden başlatma, uzun kilit, disk dolu). Sonuç: eşik = (kabul edilebilir kesinti süresi × ölçülen
üretim hızı × ölçülen ileti boyutu), bellek payıyla karşılaştırılarak. **2a bu politikayı uygulamaz; sadece ölçüm ve durum
bildirimi ekler. Production kapısı: politika tasarlanıp onaylanmadan production yok.**

## 6. Kapanış politikası: ölçülmesi gerekenler (Öneri; süre seçilmedi)
**Bugün ölçülenler:** normal `service m2dev stop` 3 sn (iki T-1 durdurması, 2026-10-07), `unexecuted_at_quit=0`;
`rc.d/m2dev` stop 30 sn bekleyip mesaj veriyor, süreçleri öldürmüyor (`/usr/local/etc/rc.d/m2dev` `m2dev_stop`); sistem
kapanışında `rcshutdown_timeout=90` sn (`/etc/defaults/rc.conf`), `kern.init_shutdown_timeout=120` sn (sysctl).
**Ölçülecek:** kapanışta kuyruk boyu (yük altında), kapanış flush'ının boşalma hızı (ifade/sn), DB yavaşken (InnoDB kilidi,
Aria kilidi) ve DB kapalıyken kapanış süresi, sistem kapanışında hangi süreden sonra süreçlerin öldürüldüğü.
**2a'da:** kapanışta kopya ve ana kuyruk aynı sınıflandırmayla işlenir; hiçbir şey **sessizce** atılmaz (kalan her ileti kayda
ve sayaca); DB kapalıysa sonsuz bekleme yok (tek geçiş). Kesin süre ölçümden sonra.

## 7. Başarısızlık kaydı (failure ledger): veri sınıflandırması
**Ham SQL kayda yazılmaz (karar).** İfadelerin taşıdığı veriler (Kanıtlı, kod):
| Veri | Örnek aile (yol) | Sınıf |
|---|---|---|
| Hesap adı (login) | item_award (`game/questlua_pc.cpp:2726`), hack_log (`game/log.cpp:113`) | kişisel / kimlik |
| Karakter adı | lonca, messenger, `change_name` (`game/log.cpp:191`), hack_log | kişisel |
| IP adresi | `log`, `loginlog2`, `hack_log`, `command_log`, `change_name` (`game/log.cpp:53, 80, 113, 191, 200, 279`) | kişisel (KVKK, A-2) |
| Ekonomi değerleri | item vnum/count/attr (`db/Cache.cpp`), oyuncu altını, depo altını (`db/ClientManager.cpp:958`), **cash/mileage** (`:3587`) | ekonomi |
| Kimlik doğrulama sırrı | **depo şifresi açık metin** (`db/ClientManager.cpp:848`; A-13) | gizli |
| GM komutu | `command_log` (`game/log.cpp:200`) | denetim |
| Serbest metin | lonca yorumu, `why` alanı | kullanıcı içeriği |

**Karar (2a ilk sürümü) — sadece metadata.** Tutulur: sorgu ailesi/sınıfı, iç sorgu/korelasyon sıra numarası, hata kodu ve
evre, hata sınıfı, deneme sayısı, bağlantı/kuyruk rolü, kuyrukta bekleme süresi, son durum, build kimliği, zaman ve gerekli
performans metadata'sı. **Tutulmaz:** hesap id, oyuncu id/pid, item id, IP, isim, ham SQL, SQL hash'i (düşük entropili değerler,
ör. 6 haneli depo şifresi, sözlükle bulunabilir), sorgu parametreleri. 2b ya da kurtarma tasarımında varlık kimliği gerçekten
gerekirse gizlilik/erişim/saklama etkisi ayrıca incelenip karar verilir.

## 7b. Mevcut ham SQL logları: hassas veri denetimi (revizyon 4, Kanıtlı kod)
Bugün başarısızlıkta ya da yavaşlıkta **ham SQL** `syserr`/`syslog`'a yazılıyor. Bu loglar 0644 (herkes okuyabilir; roadmap 1.4,
A-12) ve 7 gün (syslog) / 30 çalışma (syserr) saklanıyor. Test VM'de bugün bu satırlardan **hiç yok** (sayım, içerik basılmadı:
`QUERY_FLUSH`, `LONG INTERVAL`, `query failed`, `SLOW-*` = 0), yani yol var, olay henüz olmamış.

| Yol | Log | Ne zaman | |
|---|---|---|---|
| `libsql/AsyncSQL.cpp:298-302` | syserr | `DirectQuery` hatası | ham SQL |
| `libsql/AsyncSQL.cpp:580-581` | syserr | worker hatası | ham SQL |
| `libsql/AsyncSQL.cpp:622-623` | syslog | worker'da > 0,5 sn süren **her** ifade | ham SQL |
| `libsql/AsyncSQL.cpp:679-680` | syserr | kapanış döngüsünde hata | ham SQL |
| `libsql/AsyncSQL.cpp:704` | syslog | kapanışta kuyruğa kalan **her** ifade (`QUERY_FLUSH`) | ham SQL |
| `libsql/AsyncSQL.cpp:756` | syserr | escape tamponu yetmezse | kaynak metnin ilk 255 karakteri |
| `db/DBManager.cpp:125` | syserr | db `DirectQuery` > eşik (`[SLOW-DB]`) | ham SQL |
| `game/db.cpp:71` | syserr | game `DirectQuery` > 200 ms (`[SLOW-GAME]`) | ham SQL |

**Bu yollara düşebilecek hassas değerler (gerçek sorgu envanteri):**
- **Kimlik doğrulama:** auth giriş sorgusu `SELECT '<x>',password,… FROM account WHERE login='<login>'` (`game/input_auth.cpp:288-299`) —
  `<x>` normal yolda **şifre hash'i** (tuzsuz MySQL native hash, sözlükle kırılabilir). Bu ifade game `m_sql` worker'ında
  `ReturnQuery` ile gidiyor → hata olursa `:580`, yavaşsa `:622` yoluyla loga düşer. ("Channel service" dalı için aşağıya bak.)
- **Depo şifresi (açık metin):** `UPDATE safebox SET password='…'` (`db/ClientManager.cpp:848`) → hata `:580`, yavaşlık `:622`,
  kapanışta kuyrukta kalırsa `:704`.
- **Hesap/karakter adları, IP, GM komutları:** log `INSERT`'leri (`game/log.cpp:53, 80, 113, 129, 191, 200, 279`), item_award
  (`game/questlua_pc.cpp:2726`), lonca/messenger.
- **Ekonomi:** item satırları, altın, `cash`/`mileage` miktarları (`db/ClientManager.cpp:958, 3587`).

**2a kararı (Öneri):** bu 8 yolda ham SQL **varsayılan olarak yazılmaz.** Yerine: sorgu ailesi/sınıfı, korelasyon numarası
(`iID`), hata kodu, evre, sonuç, deneme sayısı, bağlantı rolü, süre. Olay sayısı ve sınıfı korunur (sayaç + ledger), yani
hata kanıtı budanmaz. **Aile etiketi** SQL'den türetilir: fiil + `FROM`/`INTO`/`UPDATE`'ten sonraki tablo adı; tırnaklı
dizgiler ve `\`-kaçışları atlanarak taranır, **hiçbir değer alınmaz** (ör. auth sorgusu → `SELECT account`); türetici,
şifre/isim içinde `FROM`/`INTO` geçen ve kaçış içeren düşmanca girdilerle birim testinden geçer. Ham SQL gerçekten gerekirse
**özel hata ayıklama modu** ayrı onaya getirilir; production varsayılanı olmaz.

**Yan bulgu (2a kapsamı dışı): "channel service" dalı — latent güvenlik/ölü kod borcu, bugün kanıtlanmış istismar değil.**
- Dal, kullanıcının **düz şifresini** (escape edilmiş) SQL metnine koyuyor (`game/input_auth.cpp:260-261, 267-283`); aynı
  dosyadaki "düz şifre SQL'e girmez" yorumu (`:225-226`) bu dal için doğru değil.
- **Erişilebilirlik (Kanıtlı kod, 2026-10-07):** tek giriş yolu `CG::LOGIN3` → `HandleLogin3` → `CInputAuth::Login`
  (`:163-175`); sıra `trim_and_lower` → `FN_IS_VALID_LOGIN_STRING` (`:207`, geçmezse `NOID`) → escape → `Login_IsInChannelService`
  (`:267`, tek çağrı noktası; `[` ile başlıyor mu, `:145-150`). Doğrulayıcı (`:67-142`) yalnız harf/rakam ve locale
  istisnalarını kabul ediyor: Canada `` _-.!@#$%^&*()``, Korea/YMIR `-_`, Brazil `_-=`, Japan `-_@#` — **hiçbirinde `[` yok**;
  escape `[` eklemez. Doğrulayıcıyı atlayan başka bir çağrı yolu bulunmadı (arama: `Login_IsInChannelService`,
  `FN_IS_VALID_LOGIN_STRING`, `CInputAuth::Login`).
- **Sınıf:** mevcut LOGIN3 doğrulama sırası nedeniyle normal istemci için erişilemez görünüyor → uzaktan istismar kanıtlanmadı;
  **latent güvenlik / ölü kod borcu**. Doğrulayıcı ya da yönlendirme değişirse risk geri gelir. Ayrı temizlik/güvenlik analizi
  (2a dışı; roadmap'e 2a PR'ındaki doküman güncellemesiyle girer).

**2a dışında kalan diğer ham SQL logları** (değer içerebilir, AsyncSQL hata yolu değil; ayrı küçük iş): `db/ClientManager.cpp:751`
(`SAFEBOX Query`), `:971` (`EmpireSelect`), `:2963, 3360, 3409` (`DirectQuery failed`), `db/GuildManager.cpp:1371, 1483`
(`WAR_REWARD`, hesap adları), `db/Cache.cpp:64`, `game/log.cpp:39` (yalnız `test_server`).

## 8. H-1: üç bağlantı ve silinen item (Kanıtlı, `sqlprobe` H1a–c, 3/3)
| Prob | Kurgu (gerçek `item` DDL'i, gerçek ifade biçimleri) | Sonuç |
|---|---|---|
| H1a | main meşgul (`DO SLEEP(1)`), arkasında `REPLACE X`; o sırada async `DELETE … WHERE id=X` | silme hemen uygulandı, sonra main'deki `REPLACE` geldi → **silinen item geri döndü** |
| H1b | aynı, silme direct `DELETE … WHERE owner_id=…` (oyuncu silme biçimi) | **geri döndü** |
| H1c | aynı + main'de bariyer: `ReturnQuery` sonucu gelene kadar silme yapılmıyor (`QID_PLAYER_DELETE` deseni, `db/ClientManagerPlayer.cpp:1044`) | **silinmiş kaldı** |

**Uygulamada erişilebilir mi?**
- **Async yol (`dwPID == 0`, `db/ClientManager.cpp:1546`):** game sahibi olmayan item'ı kaydetmiyor, doğrudan `ITEM_DESTROY`
  gönderiyor (`game/item_manager.cpp:437-447`, `:557-564`) → main'de o item için bekleyen `REPLACE` beklenmiyor. **Kanıt yok**
  (sahipliği değişen ya da depo/mall item'larının son sahip değeri incelenmedi).
- **Oyuncu silme (direct):** db silmeyi önce main üzerinden `QID_PLAYER_DELETE` ile yapıyor, `DELETE`'ler bu sonucun işleyicisinde →
  main'de daha önce kuyruğa girmiş bütün kaydetmeler bitmiş oluyor. H1c bu bariyeri kanıtlıyor.
- **Sonuç:** mekanizma gerçek ve deterministik; bugün bilinen yollar ya erişilemez (kanıtsız) ya bariyerle korunuyor.
  **Runtime değişikliği yok.** 2a'nın değişmezleri: (1) bağlantı başına **FIFO bozulmaz**; (2) **`ReturnQuery` bariyeri
  bozulmaz** — sonuç, önceki bütün ifadeler bitmeden (uygulandı ya da kesin başarısız) teslim edilmez; (3) ileride "takılan
  başı atla" ya da "başarısızı sona at" türü bir optimizasyon **yapılmaz**. "DELETE'i main'e taşımak" üç bağlantılı sıralamayı
  çözmez. **H-1 ayrı açık risk/test maddesi** olarak kalır; erişilebilir bir yol kanıtlanırsa ayrı iş (2c). H1c 2a'nın
  regresyon setinde.

## 9. K7: `while (!QueryLocaleSet());` (Kanıtlı, `sqlprobe` K7a–d)
**Çalıştığı yerler:** `DirectQuery` (`libsql/AsyncSQL.cpp:283`) → **game ana thread'i** (`game/db.cpp` `DirectQuery`, 19 çağrı
yeri) ve db ana thread'i; worker (`:570`) → game `m_sql`, game `LogManager`, db main/async (slot başına); kapanış döngüsü
(`:664`). `CAsyncSQL2::SetLocale` tek çağrı, döngü değil.

**Tetiklenme koşulu (kod + test):** bir ifade sırasında Connector sessizce yeniden bağlanır (yeni `thread_id`,
`mariadb_lib.c:2215-2224`); AsyncSQL bunu ancak **sonraki** ifadede görür. O ana kadar sunucu erişilemezse döngü başlar: her
turda `SET NAMES` → yeniden bağlanma başarısız → beklemeden tekrar.
| Prob | Sonuç (3 koşu) |
|---|---|
| K7a worker, 5 sn kesinti | worker kesinti boyunca **kilitli**; 5 sn'de **~1.000+ `syserr` satırı** (~200/sn); süreç CPU'su 0,2 sn (yoğun döngü değil, her tur bağlantı denemesi); sunucu dönünce ifade uygulandı |
| K7b `DirectQuery`, 5 sn kesinti | **çağıran 5,4–6,6 sn bloklandı** (kesinti + yeniden başlatma); game'de bu **ana döngünün donması** = bütün oyuncular |
| K7c kontrol: öncesinde yeniden bağlanma yok | `DirectQuery` 2002 ile **hemen** döndü (döngüye girmedi) |
| K7d Connector yeniden bağlanınca karakter seti | sunucu varsayılanı `utf8mb4`; `latin1` ile bağlanan bağlantı öldürülüp sessizce yeniden bağlandı → **`latin1/latin1/latin1` korundu** (`mariadb_reconnect` kendisi `mysql_set_character_set` yapıyor, `:2186-2192`) |

**Sonuç:** döngü hem tehlikeli hem **gereksiz** (K7d). Karar (kabul edildi): döngü kaldırılır; yeniden bağlanma sayılır ve bir
kez loglanır; süre/deneme sınırı seçilmez. "Connector karakter setini zaten kuruyor" varsayımı **regresyon testiyle korunur**;
önce/sonra karşılaştırması: normal bağlantıda karakter seti; sessiz yeniden bağlanma sonrası karakter seti (K7d); yeniden
bağlanma başarısızlığı (K7a/K7b kurgusu); `DirectQuery` yolu (K7b); worker yolu (K7a). Kabul: düzeltmeden sonra beklemesiz
döngü yok, binlerce `syserr` satırı yok, `DirectQuery` çağıranı kesinti boyunca bloklanmıyor (5–6 sn bloklama ortadan kalkar),
karakter seti her durumda `latin1`. Ayrıca bağlantı/yeniden bağlanma sonrası oturum `autocommit=1` doğrulanır.

## 10. 2a kapsamı (kabul şartlarıyla; `libsql`, game + db birlikte)
Ayrı PR; 2a test VM kabulünü geçmeden 2b'ye geçilmez. Şartlar (kullanıcı onayı, 2026-10-07):
1. **Bağlantı başına FIFO korunur; `ReturnQuery` bariyeri korunur** (bölüm 8 değişmezleri).
2. **Eski kalıcı-hata tekrar listesi kaldırılır** (1133, 1138, izin/şifre/host, tablo bozukluğu artık tekrar edilmez).
3. **Sonuç ≠ politika (bölüm 1b):** iki eksen ayrı tutulur; otomatik tekrar yalnız `TRANSIENT_CONNECTION` (ve işaretli
   aileler için bölüm 1b'deki sınıflar); `CONNECTION_CONFIG_FATAL`, `INTERNAL_PROTOCOL_STATE`, `QUERY_PERMANENT`, bilinmeyen kod →
   tekrar yok; `RESOURCE_LIMIT` (1040/1203/1226) şimdilik tekrar yok ama kalıcı sayılmaz. 100 ms aralık yalnız transient yolda, `Quit` ile kesilir, game ana thread'ini bloklamaz (`DirectQuery` tekrar etmez),
   `syserr`'e deneme başına değil durum değişimi başına satır.
3a. **Tekrar yalnız kanıtlı güvenli sınıflarda:** `rc=-1` + TRANSIENT_CONNECTION her aile; 1205/1213 yalnız `rollback_proven`
   işaretli ailelerde; belirsiz sonuç yalnız `idempotent` işaretli ailelerde. **Belirsiz sonuç varsayılan olarak kör tekrar
   edilmez.** 2a'da hiçbir aile işaretlenmez (API var, varsayılan kapalı); işaretleme aile testleriyle sonraki işlerde.
4. **S11:** async yolda sonuçlar boşaltılır.
5. **S12:** `uiSQLErrno` = iletiyi bitiren denemenin kodu.
6. **K7:** `QueryLocaleSet` döngüsü kaldırılır (bölüm 9 regresyon testleriyle).
7. **Kapanış:** kopya ve ana kuyruk sessizce kaybolmaz; kalan her ileti sayılır ve ledger'a girer; sonuç görünür ve
   deterministik. Süre/degraded kapanış politikası ayrı karar (bölüm 6).
8. **Telemetri:** kuyruk **bayt**, derinlik ve yaş; sınıf/evre sayaçları; ledger sayacı; `docs/monitoring.md` + `m2metrics.py`.
9. **Bağlantı ve yeniden bağlanma sonrası oturum durumu doğrulanır:** `autocommit=1` ve karakter seti; beklenmezse uyarı + sayaç.
10. **Failure ledger:** sadece metadata (bölüm 7), dosya izni 0600.
10a. **Ham SQL loglanmaz (bölüm 7b):** 8 yol metadata'ya çevrilir; aile etiketi değer almadan türetilir (düşmanca girdi testleri);
    özel hata ayıklama modu yok (gerekirse ayrı onay).
11. **Keyfi production eşiği/zaman aşımı eklenmez:** sınırsız kuyruk production politikası ilan edilmez; yüksek su eşiği yok;
    oyun thread'ine bloklayan geri basınç yok; sorgu atma yok; bakım/degraded davranışı yok (bölüm 5).
12. Kilitli `empty()` (K8); kopya kuyruğunu da sayan bekleyen sayısı (K4); `AddCopiedQueryCount` (K10).
13. **`CLIENT_MULTI_STATEMENTS`:** 2a içinde test edilir; gerçek çok ifade bağımlılığı yoksa **kapatılır**. Sondaki `;` tek
    başına bayrak gerektirmez (`item_award`, `game/questlua_pc.cpp:2726, 2754`). Kanıt: (a) kaynak envanteri — aynı çağrıda
    birden çok ifade var mı; (b) bayrak kapalıyken `item_award` dahil mevcut tek ifadeli sorgular çalışıyor mu; (c) S1–S12 ve
    regresyon seti değişiyor mu; ayrıca test VM'de değer içermeyen `performance_schema` digest envanteri.

## 11. 2a dışında bırakılanlar
- **2b:** db kaydetme onayı / başarısız kaydetmede önbellek durumu (2a test VM kanıtından sonra).
- Ailelerin `rollback_proven` / `idempotent` olarak işaretlenmesi (2b ve sonrası, aile testleriyle).
- H-1 (ayrı açık risk/test maddesi; runtime değişikliği yok).
- "Channel service" dalının temizliği (latent güvenlik borcu, bölüm 7b) ve 2a dışındaki diğer ham SQL logları (log hijyeni,
  bölüm 7b) — **ayrı security/log-hygiene backlog**; 2a PR'ının roadmap güncellemesinde kayda geçer.
- Kuyruk geri basıncı / degraded / bakım modu (game entegrasyonu + yük testi ölçümü).
- Kapanış süre değeri; ledger payload'ı.
- H-1 değişikliği (bölüm 8: önerilmiyor).
- Hatayı "satır yok" sayan çağıranların denetimi (`db/ClientManager.cpp:486`, `db/ClientManagerLogin.cpp:146-162`).
- `ChargeCash` için tekil işlem anahtarı (ekonomi, ayrı tasarım).
- `loginlog2` tek ifadeye indirme; game ana thread'inde `DirectQuery` performansı; Aria `lock_wait_timeout=86400` config kararı.
- D-8 (transaction'lı oyuncu kaydı), InnoDB dönüşümü.

## 12. Test matrisi
| Alan | Test | Beklenen (2a sonrası) |
|---|---|---|
| Baseline | S1–S12 (`run.sh`) | S1/S2/S3/S9/S12: 1133 kalıcı → kuyruk tıkanmaz, işaretçiler yazılır; S4/S8/S9b/S10 aynı; S5/S6/S7 aynı (aileler işaretsiz → tekrar yok, kayıt + sayaç); S11: 2014 yok, işaretçiler yazılır |
| Evre | P0–P5, P1b/P1c, G1 | `rc=-1` (teslim edilmedi) → yerinde tekrar, FIFO bekler (`stuck_ms` artar, üretici beklemez), dönünce sırayla uygulanır; `rc=1` bağlantı hatası → işaretsiz ailede tekrar yok, ledger'a `belirsiz`; evresi bilinmeyen başarısızlık güvenli sayılmaz |
| Bariyer | H1c + kesinti altında | bariyer korunur: silme, bekleyen kaydetmeden sonra |
| Belirsiz | P2 + işaretli idempotent aile / işaretsiz göreli aile | idempotent: tekrar, son durum aynı; göreli (`cash`): tekrar yok, kayıt; çift yükleme yok |
| Geri alma | R1–R5 + işaretli aile | işaretli aile 1205/1213'te tekrar → bir kez uygulanır; işaretsiz: tekrar yok |
| K7 | K7a/b/c/d önce/sonra | döngü yok; binlerce `syserr` satırı yok; `DirectQuery` kesinti boyunca bloklanmaz; karakter seti her durumda `latin1`; `autocommit=1` |
| Kapanış | S9 varyantları; DB kapalıyken `Quit` | sessiz atma yok; kalanlar sayaç + kayıt; süre sınırsız değil |
| Çok ifade | kaynak envanteri + digest envanteri; bayrak kapalıyken `item_award` biçimi (`…;`) ve S1–S12 | gerçek bağımlılık yoksa kapatılır |
| İki eksen | C1–C4, P3, G1, S11 | config/fatal ve internal hatalarda tek deneme, döngü yok, durum değişimi başına bir `syserr` satırı; transient'te 100 ms yerinde tekrar, `Quit` ile kesilir |
| Ham SQL | 8 yolu tetikleyen testler (hata, > 0,5 sn, kapanış flush'ı, escape, SLOW) + düşmanca aile türetici birim testi | loglarda ham SQL/değer yok; aile, korelasyon, kod, evre, sonuç, deneme var |
| Ledger | içerik denetimi | SQL metni, hash, parametre, hesap/oyuncu/item id, IP, isim **yok**; bölüm 7 alanları var; 0600 |
| Telemetri | `m2metrics.py` | yeni alanlar okunur, eski satırlar bozulmaz |
| Performans | `enqueue-bench`, boşta + sentetik yükte CPU A/B | üretici maliyeti ölçülür, fark kanıtlanır (eşik önceden konmaz) |
| Uçtan uca (test VM, ayrı onay) | giriş, ticaret, item, depo, lonca, kapanış; MariaDB yeniden başlatma | `sql_*.log` sayaçları; veri kontrolü |
| Envanter | `performance_schema` digest özetiyle (değer içermez) test VM'de gerçek ifade aileleri | grep envanterinin eksiklerini bulmak (bölüm 13) |

## 13. Red-team / öz inceleme
- **Kanıt kapsamı dar:** Connector 3.4.5 / Server 11.8.9 / TCP / TLS yok / `autocommit=1` / trigger yok. Değişirse prob paketi
  (`probe.sh`) tekrar koşulmalı; 2a bağlanınca `autocommit`'i doğrular.
- **Revizyon 1'de iki yanlış genelleme ve bir yanlış envanter iddiası vardı** (bölüm 0). Grep envanteri bu yüzden kanıt sayılmaz;
  değer içermeyen `performance_schema` digest özeti ile test VM'de gerçek aile listesi çıkarılmalı.
- **Evre kanıtı** Connector'ın bu sürümündeki gönderim yoluna ve yazma döngüsüne dayanıyor; sunucu tarafı üç kesilme noktası
  (P1/P1b/P1c) ile test edildi. TLS/sıkıştırma ve başka Connector sürümleri test edilmedi; değişirse prob paketi tekrar koşulur.
- **`rc=-1` ≠ geçici (çözüldü, bölüm 1b):** 1045/1049 (yapılandırma) ve 1040/1226 (`RESOURCE_LIMIT`, geçici olabilir) teslim
  edilmedi ama 2a'da tekrar edilmez. Bedeli: bu durumlar sürerken yazmalar başarısız olur (bugün de öyle; artık görünür ve
  sınıfı doğru). Tutma, kaynak/degraded politikası ayrı karar.
- **Transient listesi dar:** yalnız 2002/2006/2013(`rc=-1`). MariaDB yeniden başlarken başka bir el sıkışma kodu dönerse tekrar
  edilmez ve yazma kaybolur (görünür); MariaDB yeniden başlatma testinde ölçülecek.
- **Aile etiketi türetici** ham SQL'i tarar; bir hata değer sızdırabilir → düşmanca girdi birim testi ve "şüphede `unknown`" kuralı.
- **InnoDB 50 sn / Aria 86400 sn bekleme:** "teslim edilmedi" dışındaki her tekrar worker'ı uzun bekletebilir; kuyruk büyümesi
  bölüm 5'teki politikaya bağlı.
- **Yerinde tekrar = FIFO bekler:** girişler (yükleme) aynı main bağlantıda → DB sorununda girişler de gecikir.
- **Bariyer kırılganlığı:** ileride "başı atla" ya da "sona at" iyileştirmesi H-1'i erişilebilir yapar; değişmez olarak testte
  (H1c) tutulmalı.
- **Ekonomi:** `ChargeCash` belirsizlikte at-most-once → bakiye yüklemesi kaybolabilir (kayıtta görünür); at-least-once çift
  yükleme demek. İkisi de exactly-once değil; tekil işlem anahtarı ayrı tasarım.
- **Ledger kişisel veri taşır** (hesap/oyuncu kimliği): izin, saklama, erişim kararı A-2 ile.
- **Telemetri anlamı değişecek** (`retry` gerçek tekrar olur; yeni sınıflar): okuyucu ve doküman birlikte güncellenmeli.
- **Kapsam kayması:** 2b, degraded mod, H-1 ve çağıran denetimi 2a'ya alınmamalı.

## Kararlar (2026-10-07)
- 2a ve 2b ayrı PR; 2a test VM kabulü olmadan 2b yok.
- Ledger: 2a'da sadece metadata (bölüm 7).
- K7 döngüsü kaldırılır, regresyon testiyle (bölüm 9).
- `CLIENT_MULTI_STATEMENTS` 2a içinde test edilir; bağımlılık yoksa kapatılır.
- Kuyruk ve kapanış: 2a yalnız ölçüm ve görünürlük; eşik/süre/degraded davranışı yük testi ölçümünden sonra ayrı karar.
- H-1: ayrı açık risk/test maddesi; runtime değişikliği yok.
- 1040/1226 `RESOURCE_LIMIT` (kalıcı değil; 2a'da tekrar yok). "Channel service" dalı: latent borç, istismar kanıtı yok.
- Revizyon 4 kanıtı ayrı commit; 2a dalı bu commit'ten sonraki `main`'den açılır.
