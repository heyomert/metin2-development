# db açılışında proto kopyalama hataları (sql_mode + config izni)

- **Tarih:** 2026-10-04
- **Tür:** ortam
- **Alan:** db / runtime
- **Durum:** Taşındı → `docs/build-and-run.md` ("Veritabanı")
- **PR / commit:** — (VM'de `/usr/local/etc/mysql/conf.d/zz-acceptance.cnf`; yedeği `/root/zz-acceptance.cnf.bak`)

## Problem / hedef
`db/syserr.log`'da her açılışta ~1338 hata: `Data truncated for column 'size'` (mob_proto),
`Data too long for column 'name'/'locale_name'` (item/mob_proto).

## Kök neden / kanıt
1. Upstream kurulum belgesi `sql_mode=NO_ENGINE_SUBSTITUTION`'ı zorunlu tutuyor (`server-src/README.md:1078`);
   VM'de ayarlanmamıştı, MariaDB 11.8 varsayılan olarak strict (`STRICT_TRANS_TABLES`).
   Kod `mob_proto.size` (`enum`) alanına sayı yazıyor (`ClientManagerBoot.cpp:1379`); strict mod sorguyu reddediyor.
2. Ayar eklendikten sonra da etkisi olmadı: `GLOBAL_VALUE_ORIGIN = COMPILE-TIME`. `zz-acceptance.cnf` `600` izinliydi,
   MariaDB `mysql` kullanıcısıyla okuyamadığı için **sessizce atladı** (root olarak `--print-defaults` ayarı gösteriyordu).

## Etkisi
Oyunu etkilemiyordu: çekirdekler proto'yu db'nin belleğinden alıyor (`ClientManager.cpp:299-304`), SQL tabloları sadece
kopya (`Mirror*IntoDB`), kodda okuyan yok.

## Reddedilen yaklaşımlar
- Strict modu açık tutup kodu uyumlu hale getirmek: upstream'den ayrışır, sınırı belirsiz (başka sorgular da takılabilir).
- `bind-address=0.0.0.0` (upstream önerisi): uygulanmadı, `127.0.0.1` daha güvenli ve yeterli.

## Çözüm
`zz-acceptance.cnf`'e `sql_mode=NO_ENGINE_SUBSTITUTION`, dosya izni `644`; MariaDB + `m2dev` yeniden başlatıldı.

## Doğrulama
`sql_mode = NO_ENGINE_SUBSTITUTION`, kaynak `CONFIG`; açılışta 0 sorgu hatası; `mob_proto` 1338 kayıt = `.txt` (VM yeniden başlatmada da).

## Bir dahaki sefere tuzaklar
- Ayarı root ile `mariadbd --print-defaults` göstermesi yetmez; `mysql` kullanıcısıyla (`su -m mysql -c ...`) ya da
  `information_schema.SYSTEM_VARIABLES.GLOBAL_VALUE_ORIGIN` ile kontrol et.
- `mob_proto.size` tabloda boş: upstream `mob_proto.txt`'de SIZE sütunu bütün satırlarda boş, kopya doğru.
- Non-strict modda uzun isimler kopyada 24 byte'a kısalıyor (165 eşya); oyunu etkilemez.
