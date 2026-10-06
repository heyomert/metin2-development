# Durum

Kapsadığı commit: T-2 PR'ı (2026-10-06; taban 87457e72 = PR #19 merge). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline 87457e72..HEAD -- . ':!docs/status.md'` T-2 PR'ı dışında bir şey gösteriyorsa eski olabilir; değişikliklere bak, düzelt.

## Şu an
**T-2 / 1.9 derleme kimliği PR'da (merge bekliyor):** mekanizma kodlandı; kimlik, kurulum (korumalı dizin) ve okuyucu testleri
geçti; `archive` derlemesi VM'de derlendi. **Doğrulanmadı:** çalışan süreçlerde uçtan uca (kurulum + yeniden başlatma, ayrı
onay) ve production temiz-git FreeBSD release yolu (yok; production kurulumu bu yüzden fail-closed). Ayrıntı:
`docs/build-and-run.md` → "Derleme kimliği ve kurulum", `docs/worklog/2026-10-06-build-identity.md`. A-12 analizi ve kanıt
düzeltmeleri merge (PR #18, #19); `m2build` parola girişi kilitli.
DB standardı çalışması (plan: `docs/engineering/db-standard.md`). **Adım 1c bitti (PR #17 merge)** (`docs/engineering/db-step1c-sql-counters.md`, `docs/worklog/2026-10-06-sql-counters.md`): davranış önce/sonra aynı, sayaçlar gerçekle uyuşuyor, `log/sql_*.log`. Yeni bulgu S12 (`uiSQLErrno` tekrar sonrası temizlenmiyor, login-by-key) düzeltme adımında. 1b (PR #15, `m2dev-dbstat` servis; bekleyen: 1 günlük gerçek boyut) ve 1a (PR #14, AsyncSQL hatalarının baseline'ı: takılan kuyruk, kaybolan 1205/1213/2013 yazmaları, kapanışta kayıp) bitti. DB yedeği (PR #13) ve sağlık kaydı (PR #12) test VM'de çalışıyor. Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **T-2** PR incelemesi → merge sonrası gerçek commit'ten `archive` paketi → test VM'de uçtan uca (stop → consistent yedek → `m2dev-install-binaries.sh --policy test-vm` → start → giriş + `build=`; ayrı onay)
2. **T-1** `syserr.log` yeniden başlatmada korunur (etki analizi). T-1 ve T-2 AsyncSQL düzeltmesi **devreye alınmadan önce** (`docs/roadmap.md`)
3. **AsyncSQL reliability fix** (1a testleri tersine dönmeli)
4. **Şema yönetimi / migration standardı** (sürümlü migration, makinece okunur şema sürümü, InnoDB kuralı)
5. **InnoDB dönüşümü** (migration olarak; `INSERT DELAYED` kaldırma ile birlikte) → yük testiyle kabul
Bağımsız: **T-3** disk boş alanı (dbstat'a küçük ekleme), **T-4** süreç bazında servis durumu (1.7 ile), `db.core` (2026-10-05) incelemesi, **A-12** düzeltmesi (analiz: `docs/engineering/a12-permissions.md`; VM izinleri, `perms.py`, servis kullanıcısı ayrı etki analizi ve onayla), production temiz-git release yolu (git'li derleme makinesi, ayrı onay), tekrar üretilebilir derleme (binary'ler mutlak kaynak yolu taşıyor; teknik borç). Ayrı onay bekleyen: ticaret + geçici trigger testi.

## Senden bekleyen kararlar
- `heart_idle` gecikmede fazladan pulse sayıyor (teknik borç): yük testinden önce etki analizi yapılsın mı
- Dakika hassasiyetinde geri dönüş (binlog + PITR) gerekli mi
- Görev listesinde uzun adların taşması (`client/assets/root/interfacemodule.py:1375`); `vendor/freetype-2.13.3` silinsin mi; client DPI
- A-1 hile tepki politikası; özellik listesi (F-1 dahil); production sunucu sağlayıcısı (G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil), `client/config/metin2.cfg`'deki kişisel ayarlar

## Ortam
Test VM: `pf` aktif, `conf/` 750/640. `m2dev_dbstat` servisi açık; geliştirme dizini `/root/dbstat-dev`. **game/db = 1c binary'leri** (`/root/build-1c`, kaynak `/root/src-1c` = PR #17 kodu), önceki binary'ler `/root/pre-1c-2026-10-06/` (sha256 ile). `qc` → 4 Ekim derlemesi. Windows'u yeniden başlatmadan önce VM'i kapat (`ssh bsd shutdown -p now`).
VM `/root` geçici güvenlik kopyaları (silmek için sor): `mysql-cold-backup-20261005-postcrash`, `aria-log-corrupt-20261005`, `mysql-cold-pre-drill-20261005`.
GM hesabı `admin` (şifre repo dışında). Client derlemesi `/m:1` ile. `Metin2.exe` yönetici yetkisiyle çalışır.
