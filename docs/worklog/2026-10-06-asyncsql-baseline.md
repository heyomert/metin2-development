# AsyncSQL davranış baseline'ı: hata enjeksiyonu testleri (DB adım 1a)

- **Tarih:** 2026-10-06
- **Tür:** karar / ortam (test aracı + analiz)
- **Alan:** server-src (libsql, sadece okuma) / tools / docs
- **Durum:** Aktif
- **PR / commit:** [#14](https://github.com/heyomert/metin2-development/pull/14)

## Problem / hedef
Aria → InnoDB dönüşümünden ve AsyncSQL düzeltmesinden önce bugünkü `libsql/AsyncSQL` davranışını **kanıtla** kaydetmek;
ileride yapılacak düzeltmenin değişmez regression baseline'ı olsun. Analiz ve plan: `docs/engineering/db-standard.md`,
`docs/engineering/db-step1-measurement.md`. Bu iş game/db/libsql davranışını değiştirmez.

## Kök neden / kanıt
Araç: `tools/sql-reliability/` (`sqlrt.cpp` `CAsyncSQL`'i doğrudan kullanır, `run.sh` geçici MariaDB kurar). MariaDB Server
11.8.9, Connector/C 3.4.5 (repodaki gömülü statik kütüphane). 12 senaryo × 10 tekrar, **12/12 deterministik**. Kanıt
sırası: DB'deki işaretçi satırlar, client'ın `syserr.log` satırları, kuyruk sayaçları. Ayrıntılı tablo:
`db-step1-measurement.md` → "1a sonuçları".

Baseline olarak korunacak gözlemler:
- **Retry pratikte çalışmıyor:** tek sorguluk partide listedeki hata (1133) hiç tekrar edilmez, sorgu kopya kuyruğunda takılır,
  hata kalksa bile yeni sorgu gelene kadar denenmez (S1); çok sorguluk partide parti boyu kadar denenip takılır (S2);
  kalıcı hatada kuyruk süresiz tıkanır, her yeni partide deneme sayısı büyür (S3: 1 → 3 → 6 → 10).
- **1205 ve 1213 yazmaları kaybolur:** atılır, kilit kalkınca tekrar edilmez (S5, S6; S6'da `Innodb_deadlocks=1`).
- **2013:** sorgu sırasında bağlantı kesilince sorgu uygulanmaz ve atılır (S7).
- **`KILL` ile 2006 oluşmaz:** boşta kesilen bağlantıda Connector/C (`MYSQL_OPT_RECONNECT`) sessizce yeniden bağlanıp
  gönderir (S8). Koddaki 2006 retry dalı pratikte nadiren çalışır.
- **`CountQuery()` kopya kuyruğunu görmez:** `q=0` iken `cq=4` (S9); `CountQueryFinished()` atılan sorguları da sayar (S7).
- **Takılı kopya kuyruğu kapanışta kaybolur:** 3 işaretçinin 3'ü yazılmadı (S9); normal 200'lük birikim yazıldı (S9b).
- **Async yoldan sonuç döndüren ifade bağlantıyı bozar:** `SELECT 1` sonrası bütün yazmalar 2014 ile atıldı, düzelmedi (S11).
  Bugün üretimde böyle çağrı yok (arama: `game`/`db` async yollarında `SELECT/SHOW/CALL` yok).
- **`"AsyncSQL: retrying"` log'u retry garantilemez:** tekrar olmadan yazılıyor (S1, S2).
- **MariaDB:** açık transaction'da 1205 sadece ifadeyi, 1213 bütün transaction'ı geri alır (S10).

## Reddedilen yaklaşımlar
- `KILL` ile 2006 üretmek (oluşmadı, S8). `ALTER ... NOT NULL` ile 1138 (katı olmayan modda sadece uyarı ve NULL→0, katı
  modda 1265), `ADD PRIMARY KEY` (NULL→0), MyISAM dosya izni (1036): retry listesindeki bir hatayı üretmiyor.
  Seçilen: `SET PASSWORD FOR ghost@localhost` → 1133 (listede; kullanıcı oluşturulunca aynı ifade başarılı).
- İlk S6 tasarımı (`d` tablosunda aralık güncellemesiyle ağırlaştırma) beklenmedik satırları kilitledi, kilitlenme oluşmadı;
  ağırlık ayrı tabloya satır ekleyerek verildi ve kilitlenme `Innodb_deadlocks` ile kanıtlandı.

## Çözüm
Test aracı + belgeler; üretim kodu değişmedi. Araç fail-closed: hiçbir yazmadan önce (1) yönetici bağlantısının datadir'i
`/var/tmp/m2sqlrt/`, (2) `run.sh`'ın her çalıştırmada yazdığı rastgele token o sunucuda, (3) TCP üzerinden bağlanan AsyncSQL
aynı sunucuda olmalı; `run.sh` port kullanımdaysa başlamaz. Her çalıştırmada öz-test: token yok / yanlış token / canlı
sunucunun soketi → üçü de reddedildi; `RT_PORT=3306` → başlamadı; canlı sunucuda önce/sonra `rt` veritabanı, koruma
veritabanı ve `ghost` kullanıcısı yok.

## Doğrulama
`sh tools/sql-reliability/run.sh` (VM, root): koruma öz-testi 3/3 ret, 12 senaryo 10/10 aynı. Korumalı araçla tekrar:
sonuçlar ilk baseline ile aynı.

## Bir dahaki sefere tuzaklar
- `AsyncQuery`'ye sonuç döndüren ifade verme (S11); test aracında worker'ı meşgul etmek için `DO SLEEP` kullan.
- MariaDB'de `cte_max_recursion_depth` yok; `max_recursive_iterations`.
- Ticaret/trigger testi bu işe dahil değil; ayrı onay bekliyor.
