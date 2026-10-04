# loginlog2.is_gm: kod 'Y'/'N' yazıyor, şema int

- **Tarih:** 2026-10-04
- **Tür:** düzeltme / karar
- **Alan:** db (şema)
- **Durum:** Aktif
- **PR / commit:** https://github.com/heyomert/metin2-development/pull/2

## Problem / hedef
Strict modda giriş kaydı hiç yazılmıyordu (`Incorrect integer value: 'Y' for column is_gm`); `sql_mode` düzeltilince yazıldı
ama `is_gm` her zaman 0 — GM girişleri ayırt edilemiyor.

## Kök neden / kanıt
- `server-src/src/game/log.cpp:279-281`: `ch->IsGM() ? "'Y'" : "'N'"`.
- `server/sql/log.sql:388` (düzeltme öncesi): `is_gm int(11)`. Upstream'de ilk commit'ten beri böyle, hiç değişmemiş.

## Reddedilen yaklaşımlar
- **Kodu `1/0` yazacak şekilde değiştirmek** (ilk öneri buydu): bilgisayardaki 6 bağımsız Metin2 kaynağının hepsi
  (Anka2, lorenzo, MartySama 5.8/5.9, 2008–2010 TR, Andesia) `'Y'/'N'` yazıyor; MartySama şeması
  `varbinary(20) COMMENT 'contains: Y, N'`. Dışarıda kalan taraf m2dev şeması, kod değil.

## Çözüm
`server/sql/log.sql`: `is_gm enum('Y','N')`. Mevcut DB: `UPDATE ... SET is_gm=NULL` (bozuk 0'lar bilgi taşımıyor) +
`ALTER TABLE ... MODIFY is_gm enum('Y','N')`. C++ değişmedi.

## Doğrulama
Gerçek GM girişinde yeni kayıt `is_gm = 'Y'`; çekirdek log'larında `loginlog2` hatası yok.

## Bir dahaki sefere tuzaklar
- Diğer fork'lar karşılaştırma için faydalı ama sadece bu repodaki bulguyu desteklemek için.
- `ALTER TABLE` transaction ile geri alınamaz; önce `mysqldump` yedeği al.
