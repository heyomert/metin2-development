# Derleme kimliği ve game+db kurulumu (roadmap 1.9 / T-2)

- **Tarih:** 2026-10-06
- **Tür:** özellik / karar / ortam
- **Alan:** server-src / build / runtime / tools
- **Durum:** Aktif
- **PR / commit:** —

## Problem / hedef
Hangi binary/commit'in çalıştığı kesin bilinmiyordu: game hiçbir zaman sürüm yazmıyordu, db `db revision: unknown`. AsyncSQL
düzeltmesinin önce/sonra karşılaştırmasında hangi binary'nin ölçüldüğü kanıtlanmalı. İstenen: runtime git'e bağlı değil;
tam commit + dirty + kaynak türü binary'ye gömülü; telemetride görünür; kurulan tam dosya SHA-256 ile kayıtlı; production
fail-closed. Kullanım: `docs/build-and-run.md` → "Derleme kimliği ve kurulum".

## Kök neden / kanıt
- Sürüm **configure anında** `git describe` ile hesaplanıyordu (`server-src/CMakeLists.txt`, eski 48-74); VM'de git yok
  (`GIT_EXECUTABLE-NOTFOUND`) ve kaynak git'siz kopyalanmış → `unknown`. Yeniden configure edilmeden yeni commit derlenince
  eski değer kalırdı.
- game'de makro adı uyuşmuyordu: CMake `GIT_DESCRIBE` tanımlıyor, `game/version.cpp` `GIT_DESCRIBE_VERSION`'a bakıyordu →
  git olsa da `unknown`.
- **Yanlış pathspec tuzağı (test edildi):** CMake `server-src` içinden çalışırken `git status -- server-src` her durumda
  "temiz" döndürür (yol `server-src/server-src` olur). `-C <SRC_DIR> -- .` kullanılıyor.
- **Yok sayılan ama derlenen kaynak:** `server-src/.gitignore` `*_bk`/`*.bak` kalıplarıyla örn. `src/game/old_BK/x.cpp`'yi
  yok sayar; `GLOB_RECURSE` onu derler → `--ignored=matching` şart. Hariç tutma pathspec'leri (`:(exclude)build` vb.)
  `--ignored` ile toplanan dizin satırını süzmüyor → sadece bu derlemenin kendi `CMAKE_BINARY_DIR`'i çıktıdan süzülür.
- **CMake regex'te `{n}` yok:** `^[0-9a-f]{40}$` geçerli bir commit'i reddediyordu; `string(REPEAT)` ile kuruldu (test
  yakaladı).
- **FreeBSD `make` saniye çözünürlüğü:** kimlik başlığı önceki `version.cpp.o` ile aynı saniyede yeniden yazılınca nesne
  "eski değil" sayıldı ve binary eski commit'i taşıdı (ölçüldü: başlık `.624`, nesne `.296`, aynı saniye). Başlık sadece
  değiştiğinde ve içinde bulunulan saniye geçtikten sonra yazılıyor; 3 hızlı değiştir/geri al turunda doğru.
- **Saat farkı:** Windows saati VM'den ~4,7 dk ileride (VM'de `ntpd` kapalı). `git archive` zamanları commit zamanı yazar →
  `tar -m` olmadan açılan kaynaklar "gelecekte" kalıp her derlemede yeniden derlendi. Açma `tar -xzmf`.
- **Tekrar üretilemiyor (ölçüldü):** aynı arşiv iki dizinde → game/db SHA-256 farklı; binary'ler mutlak kaynak yolu taşıyor
  (game'de 123). Dosya ↔ kimlik bağı `deploy.log` ile; `-ffile-prefix-map` ileride bir seçenek.
- **FreeBSD `mv`** hedef dizinse içine taşır ve başarılı döner → installer hedefte dizini reddeder. FreeBSD `sh`'ta `$RANDOM`
  yok.

## Reddedilen yaklaşımlar
- **Production'da git'e zorunlu bağlılık (A):** runtime'ın git'e ihtiyacı yok; git sadece derleme makinesinde.
- **Kendi dosya özeti manifestimiz (B):** gereğinden karmaşık; `archive` production kaynağı sayılmıyor.
- **`archive dirty=0`'ı production'a kabul etmek:** açıldıktan sonraki değişikliği ve sahte `SOURCE_COMMIT`'i göremiyor (test
  2). **`injected`'ı production'a kabul etmek:** doğrulanmamış beyan.
- **`built_at` gömmek:** aynı kaynaktan farklı binary; tam dosya kanıtı SHA-256.
- **Installer'a test kancası:** hatalar gerçek (`chflags schg`, dizin olan `BUILD`, gerçek süreç).

## Çözüm
`server-src/cmake/BuildIdentity.cmake` (her derlemede, değişince yazar), `SOURCE_COMMIT` + `.gitattributes export-subst`,
`src/common/build_identity{,_impl}.h` (işaret `M2BUILD|…|END`, `version.txt`/`VERSION.txt`, `BUILD:` syslog satırı,
`M2BuildFields()`), game sağlık ve SQL `sum` satırlarına `build= build_dirty= build_src=` (+79 B/satır),
`m2metrics.py` derlemeyi ve değişikliğini gösterir, `deploy/freebsd/m2dev-install-binaries.sh` (doğrula → aşama → yedek →
kur → SHA → `BUILD`/`deploy.log`, her adımda geri alma; production sadece `src=git dirty=0`).

## Doğrulama
- `tools/build/test-build-identity.sh` (Windows, git + CMake 3.31): 24 kontrol PASS — git temiz/değişmiş/izlenmeyen/yok
  sayılan-derlenen/derleme dizini/kardeş dizin/sadece `client/`/başka dizinden çalıştırma; naif pathspec'in 0 döndürdüğü
  kontrol; archive (commit, modlar `0644`/`0755`, açıldıktan sonra değişiklik ve sahte SHA sınırları, bozuk değer), injected,
  none (görünür uyarılar), başlığın sadece değişince yazılması.
- VM, gerçek derleme (`archive`, temiz dizin): 0 hata, 372 uyarı (baseline), değişen dosyalarda uyarı yok; game/db
  işaretleri `component` dışında aynı; değişiklik yokken 0 dosya, commit değişince sadece 2 `version.cpp` derlendi; `none`
  görünür uyarı verdi.
- `deploy/freebsd/test-install-binaries.sh` (VM, korumalı dizin): PASS — production'da archive/injected/none/dirty reddi;
  işaret yok/bozuk/çift/yanlış bileşen; game≠db commit ve aynı commit+biri dirty; çalışan süreç; ikinci `rename` hatasında
  ve kanıt kaydı yazılamadığında eski çiftin birebir dönmesi, `installed` satırı yok; başarıda SHA ↔ `BUILD` ↔ `deploy.log`,
  modlar `755/755/644/640`, herkese yazılabilir yok. Gerçek ELF'lerle: production archive'ı reddetti, test-vm kurdu,
  `prev_game_sha256` = değiştirilen 1c binary'si.
- `tools/metrics/test_m2metrics.py`: gerçek eski satırlar okunuyor; yeni satırlar derlemeyi gösteriyor; dönem içi değişiklik
  listeleniyor; mevcut alanlar değişmiyor.
- **Henüz doğrulanmadı:** çalışan süreçlerde `version.txt` ↔ telemetri ↔ `deploy.log` (VM'de kurulum + yeniden başlatma, ayrı
  onay); production temiz-git release yolu (git'li derleme makinesi, ayrı onay).

## Bir dahaki sefere tuzaklar
- `git status` pathspec'i CMake'in çalışma dizinine göredir; `-C` ile çalıştır.
- FreeBSD `make` saniye çözünürlüğünde karşılaştırır; üretilen başlıkların zamanına dikkat.
- Arşivi VM'de `tar -m` ile aç; saat senkronu yokken dosya zamanları güvenilmez.
- Bu ortamda Bash heredoc'u `\\n` ve `\\0` kaçışlarını bozuyor; düzenleme betiklerini dosyaya yaz.
