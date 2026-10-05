# Hesap şifreleri (K-2): mevcut durum doğrulandı, A uygulandı, B planlandı

- **Tarih:** 2026-10-05
- **Tür:** karar
- **Alan:** server (auth), db
- **Durum:** Aktif — A ✅ (sadece doküman), B ☐ planlandı (aşağıda), C → Faz 3
- **PR / commit:** — (doküman)

## Problem / hedef
Roadmap K-2 "Kritik" olarak duruyordu. Gerçekten değiştirilmesi gerekiyor mu, mevcut durum Metin2'ye göre doğru mu?

## Kanıt
- **Kontrol:** SQL'de `PASSWORD('<şifre>')` üretilip saklanan değerle `strcmp` (`server-src/src/game/input_auth.cpp:268`, `game/db.cpp:363`).
- **Saklama:** `*` + büyük harf hex(SHA1(SHA1(şifre))), 41 karakter, tuzsuz. VM'de 2 hesap bu biçimde. Sunucu kodu hesap
  oluşturmuyor / şifre değiştirmiyor (ileride web sitesi yapacak).
- **Metin2 standardı:** 7 kaynağın hepsi aynı biçimi saklıyor (Anka2, MartySama 5.8/5.9, lorenzo, Andesia, 2008–2010 TR).
- **Ağ:** şifreli kanal (`game/SecureCipher.cpp`, libsodium kx + XChaCha20-Poly1305). **Çevrimiçi tahmin:** IP başına 5 hata → 60 sn
  (`input_auth.cpp:15-16`, `:187`). **Log:** oyun kodu oyuncu şifresini yazmıyor.
- **`PASSWORD()`:** MySQL 8.0'da kaldırıldı (dev.mysql.com, "What Is New in MySQL 8.0"). MariaDB'de var ama "uygulamalarda kullanım
  için değil" ve `old_passwords`'e bağlı (mariadb.com, PASSWORD). Upstream MariaDB 11.8 istiyor (`server-src/README.md:873`).
  VM: `old_passwords=0`, `LENGTH(PASSWORD('x'))=41`, `general_log=0`.
- **Topluluk (`@fixme138`):** lorenzo/Andesia özeti C++'ta (`mysql_hash_password` → libmysqlclient `make_scrambled_password`),
  MartySama SQL'de `CONCAT('*',UPPER(SHA1(UNHEX(SHA1(…)))))` ile üretiyor. Saklama biçimi değişmiyor.
- **"Channel service" dalı** (`input_auth.cpp:246-262`, kullanıcı adı `[` ile başlarsa şifreyi özetlemeden karşılaştırır):
  erişilemez — kullanıcı adı kontrolü (`:206`, `FN_IS_VALID_LOGIN_STRING`) önce çalışır ve `[`'e hiçbir locale'de izin vermez.

## Karar
| | Ne | Neden |
|---|---|---|
| **A** ✅ | Kısıtlar belgelendi: MariaDB, `old_passwords=0`, `general_log` kapalı | Bugün doğru çalışıyor; tehlike sessizce bozulması |
| **B** ☐ | Aynı biçimi C++'ta üret | MySQL 8 / ayar değişikliği girişi bozamaz; şifre SQL metnine gitmez. Veri değişmez |
| **C** → Faz 3 | Modern hash (argon2id, libsodium projede var) | DB sızıntısına karşı asıl koruma; ama veri taşıma + web sitesi uyumu + tek thread'li auth'ta yoğunluk riski. Hesap sistemiyle birlikte |

## B planı (onay bekliyor)
**Değişiklik:** `input_auth.cpp:268` `SELECT PASSWORD('%s'),password,…` → `SELECT '%s',password,…`, ilk `%s` = C++'ta hesaplanan özet.
Yardımcı fonksiyon (ör. `game/utils.cpp`): `"*" + UPPER(HEX(SHA1(SHA1(şifre baytları))))`.

**SHA1 kaynağı:** MariaDB connector'ının platformdan bağımsız `ma_hash_new(MA_HASH_SHA1)/ma_hash_input/ma_hash_result`
(`vendor/mariadb-connector-c-3.4.5/include/ma_crypt.h:53-87`). `game` zaten `libmariadbclient.a`'ya bağlı ve bu semboller içinde
(`nm`: `T ma_hash_new` …); arka uç VM'de OpenSSL (`game` → `libcrypto.so.35`). Yeni kütüphane yok.
**Kullanılmayacak:** `ma_make_scrambled_password` — eski 16 karakterlik biçim üretir (`libmariadb/ma_password.c:126-130`); girişleri bozar.

**Zorunlu testler (kod devreye alınmadan önce):**
1. Küçük test programı, **oyunun DB bağlantı karakter setiyle** (`libsql/AsyncSQL.cpp:49`): yardımcı fonksiyonun çıktısı ile
   `SELECT PASSWORD(x)` birebir aynı mı — boş değil kısa/uzun (≤ `PASSWD_MAX_LEN`), rakam, sembol, **Türkçe karakter (ç ğ ı ö ş ü İ)**,
   boşluk, tırnak. Türkçe karakterler kritik: şifrede karakter kısıtı yok (`input_auth.cpp:206` sadece kullanıcı adını kontrol ediyor),
   DB sunucusu `utf8mb4`, tablo `ascii`. Herhangi bir fark → B durur.
2. Derle, sadece `game`'i devreye al; doğru şifre girer, yanlış şifre `WRONGPWD`, hız sınırı çalışıyor.
3. Mevcut hesaplar (değişmeyen veriyle) girebiliyor.

**Risk:** Orta (giriş yolu). **Geri dönüş:** önceki `game` binary'si. **Kapsam dışı:** channel-service dalı (erişilemez), C.

## Bir dahaki sefere tuzaklar
- Topluluk düzeltmelerini birebir kopyalama: aynı adlı fonksiyon farklı kütüphanede farklı biçim üretebilir.
- `general_log`'u hata ayıklama için açarsan, B yapılana kadar giriş şifreleri oraya yazılır.
