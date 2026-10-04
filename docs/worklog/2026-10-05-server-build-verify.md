# Server derleme süreci doğrulandı; çalışan binary'lerde MariaDB kütüphanesi farklı ayarla derlenmiş

- **Tarih:** 2026-10-05
- **Tür:** ortam
- **Alan:** build
- **Durum:** Taşındı → `docs/build-and-run.md` ("Server binary'lerini derleme")
- **PR / commit:** — (sadece doküman)

## Problem / hedef
K-3'ün kod adımından önce, VM'de server binary'lerinin nasıl derlendiğini kanıtla öğrenmek (roadmap A-9).

## Kök neden / kanıt
- Çalışan `game`/`db`/`qc` ile `/usr/local/m2dev-acceptance/build/server-freebsd/bin/` içindekiler aynı (sha256).
- VM kaynak ağacı repodaki `server-src` ile aynı (460 dosya, CR'ler atılarak içerik özeti).
- O dizinde `cmake --build` 2 sn'de bitti: hiçbir şey derlenmedi (güncel). Bu, derlemenin **çalıştığını** kanıtlamıyor.
- Ayrı dizinde sıfırdan (`/root/build-verify`, `-DCMAKE_BUILD_TYPE=Release`): yapılandırma 12 sn, derleme 116 sn, 0 hata.
- Yeni binary'ler ~1 MB küçük. Sunucu kodu ayarları aynı (`-O3`, clang 19.1.7, aynı cache değerleri). Fark tek bir yerde:
  vendor `libmariadbclient.a` — eski dizinde `-O2 -g` (190 debug bölümü, 3,5 MB), yenide `-O3` (0 debug bölümü, 0,9 MB).
  `flags.make` (`vendor/mariadb-connector-c-3.4.5/libmariadb/CMakeFiles/mariadb_obj.dir/`) iki dizinde farklı.
  Eski dizinin neden böyle olduğu kayıtlı değil (Unverified).

## Reddedilen yaklaşımlar
- "Hiçbir şey derlenmeden geçen `cmake --build` = derleme doğrulandı" demek: yanıltıcı olurdu, sıfırdan derleme yapıldı.
- Yeni binary'leri hemen devreye almak: kod değişikliği yokken gereksiz risk; ilk devreye alma K-3 kod adımında yapılacak.

## Doğrulama
Komutlar ve çıktılar yukarıda. Çalışan sunucuya dokunulmadı; çalışan binary'lerin yedeği VM `/root/build-baseline-2026-10-05/`.

## Bir dahaki sefere tuzaklar
- Aynı kaynaktan derlenen binary'ler farklı boyutta çıkarsa önce bölüm boyutlarına (`size`, `readelf -SW`) ve kütüphanelere bak.
- FreeBSD `awk`'ta `strtonum` yok; hex boyut toplamak için başka yol kullan.
- İlk devreye almada MariaDB kütüphanesi `-O2 -g` → `-O3` değişmiş olacak: giriş, karakter yükleme ve kaydetme test edilmeli.
