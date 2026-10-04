# loginlog2.playtime: TIMEDIFF int alana SSDDss olarak yazılıyor

- **Tarih:** 2026-10-04
- **Tür:** düzeltme / karar
- **Alan:** db (şema)
- **Durum:** Aktif
- **PR / commit:** https://github.com/heyomert/metin2-development/pull/3

## Problem / hedef
1 dakikadan uzun oturumların süresi yanlış: 2 dk 10 sn → `210`, 1 sa 5 dk 12 sn → `10512`.

## Kök neden / kanıt
- `server-src/src/game/log.cpp:294`: `playtime=TIMEDIFF(logout_time,login_time)` → `TIME` döner.
- `server/sql/log.sql:396` (düzeltme öncesi): `playtime int(11)`. MariaDB `TIME`'ı int'e çevirirken ayraçları atıyor.
- Oyunun 3 sorgusu (`log.cpp:279-294`) tablonun kopyası üzerinde çalıştırılarak kanıtlandı: 130 sn → 210.

## Reddedilen yaklaşımlar
- **Kodu `TIMESTAMPDIFF(SECOND, …)` yapmak (B seçeneği):** bütün Metin2 kaynakları `TIMEDIFF` kullanıyor; kod değişikliği,
  derleme ve ayrışma gerektirir. Saniye cinsinden istatistik ihtiyacı doğarsa `time` → saniye dönüşümü kayıpsız.

## Çözüm
`server/sql/log.sql`: `playtime time NOT NULL DEFAULT '00:00:00'`. Mevcut DB: `ALTER TABLE ... MODIFY playtime time ...`.
Dönüşüm kayıpsız; eski yanlış değerler de doğruya döner (`210` → `00:02:10`).

## Doğrulama
Kopya tabloda 130 sn → `00:02:10`, 3912 sn → `01:05:12`, uyarı yok. Gerçek girişte 95 sn → `00:01:35`.

## Bir dahaki sefere tuzaklar
- `loginlog2`'ye süre **sadece menüden çıkışta** yazılır (`cmd_general.cpp:277-286`); X ile kapatma, çökme, kick ve sunucu
  kapanması `INVALID` bırakır. Bu tablo oturum istatistiği için tek başına yetersiz. Oyuncu toplam süresi `player.playtime`'da.
- `time` üst sınırı 838:59:59 (~35 gün) / oturum.
- `log.sql`'deki diğer `playtime` sütunları (300, 364) koddan saniye alıyor, doğrular.
