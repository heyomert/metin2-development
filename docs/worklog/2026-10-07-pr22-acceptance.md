# PR #22 (AsyncSQL 2a) test VM kabul testi ve regresyon tabanı

- **Tarih:** 2026-10-07
- **Tür:** karar / ortam
- **Alan:** server-src / runtime / tools
- **Durum:** Aktif
- **PR / commit:** https://github.com/heyomert/metin2-development/pull/22 (merge `ee0a12a66`)

## Problem / hedef
SQL katmanını değiştiren yüksek riskli bir PR'ı merge'den önce gerçek client'larla, test VM'de, exact head ile kabul etmek
ve bu senaryoları gelecekteki değişiklikler için kalıcı bir regresyon tabanına çevirmek.

## Kök neden / kanıt
- **Kabul:** exact head `88912c7ab` `git archive` ile derlendi, installer ile kuruldu; önce eski binary (`8fbec589`) ile aynı
  senaryolar koşuldu. Sonuç: giriş, karakter oluşturma, ticaret, depo, lonca, kayıt; MariaDB kapalıyken karakter oluşturma
  (güvenli başarısızlık), aktif savaş/drop/EXP/yang, çekirdekler arası warp ve iki client ticaret; kapanış ve log güvenliği
  PASS. Ayrıntı: PR #22 açıklaması "Test VM acceptance", senaryolar `docs/engineering/regression-baseline.md`.
- **Bulunan, 2a'dan bağımsız:** depo 1062 (A-19), `RESULT_SAFEBOX_CHANGE_SIZE` cevabı (A-20), `unknown header 769` (A-21,
  Unverified), uzun kesintide warp bekleme sınırı (A-22, Unverified).
- **Kendi hatam (düzeltildi):** depo 1062 için önce "eski kod bunu sonsuza dek tekrar edip kuyruğu kilitlerdi" dedim; eski
  kaynakta tekrar listesi belirli kodlarla sınırlı (1062 yok). Eski binary de ifadeyi düşürüyordu, ama ham SQL ile logluyordu.

## Reddedilen yaklaşımlar
- **Kalıcı hata için `SET GLOBAL read_only=1`:** bütün yazma yollarını etkiler; kullanıcı onaylamadı. Kalıcı hatada tekrar
  olmaması gerçek süreçte doğal yolla (depo 1062) görüldü; yapılandırma hataları için sqlprobe C1–C4.
- **Bütün regresyon setini her PR'da zorunlu tutmak:** gereksiz maliyet; `change-impact.md` §7 garantiye göre seçer.
- **Tek koşunun sayılarını eşik yapmak** (27 sn, 974 bayt…): ürün sınırı değiller.

## Çözüm
- `docs/engineering/regression-baseline.md` (RB-01…RB-14), `change-impact.md` §7 (değişiklik → senaryo ailesi),
  `tools/acceptance/outage.sh` (fail-closed kesinti + kanıt aracı).

## Doğrulama
- `outage.sh`: sözdizimi ve bütün reddetme yolları test VM'de (MariaDB'ye dokunmadan); SQL maskelemesi eski ve yeni satır
  biçimleriyle. Gerçek bir kesintiyle uçtan uca denemesi ayrı onayla.

## Bir dahaki sefere tuzaklar
- **"Şimdi" sinyali:** agent bir komutun bitmesini beklerken yazdığı metin kullanıcıya hemen ulaşmayabilir. Kesintiyi arka
  planda başlat, kapandığını doğrula, mesajı bitir; MariaDB'yi araç kendi geri başlatsın (ilk ticaret denemesi bu yüzden kaçtı).
- **Kesintinin sonu oyundan anlaşılmaz:** oyun döngüsü DB'yi beklemiyor. "Kesintide warp" gibi adımlar zaman damgasıyla
  doğrulanır (ilk warp denemesi aslında DB döndükten sonraydı).
- **DB satırı ≠ son durum:** db oyuncu verisini önbellekte tutar; tekrar girişteki değerler `[PLAYER_LOAD] … gold` kaydından,
  kalıcılık önbellek yazıldıktan sonra `player` tablosundan doğrulanır (bu testte birkaç dakika).
- **T-1 arşivi:** yeniden başlatmadan sonra eski çalışmanın syserr'i `log/syserr_*.log`'da; önce/sonra sayımında unutulmasın.
- **`SHOW GRANTS` parola özetini basar;** `IDENTIFIED BY` kısmını filtrele.
