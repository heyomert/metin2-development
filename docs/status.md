# Durum

Kapsadığı commit: PR #17 (1c) ve #18 (A-12 analizi) merge'ü (2026-10-06; taban 18180f2f). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline 18180f2f..HEAD -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir; değişikliklere bak, düzelt.

## Şu an
DB standardı çalışması (plan: `docs/engineering/db-standard.md`). **Adım 1c bitti (PR #17 merge)** (`docs/engineering/db-step1c-sql-counters.md`, `docs/worklog/2026-10-06-sql-counters.md`): davranış önce/sonra aynı, sayaçlar gerçekle uyuşuyor, `log/sql_*.log`. Yeni bulgu S12 (`uiSQLErrno` tekrar sonrası temizlenmiyor, login-by-key) düzeltme adımında. **Adım 1b bitti (PR #15):** bağımsız, salt okunur MariaDB/OS toplayıcısı `m2dev-dbstat` test VM'de servis olarak çalışıyor (`m2stat`, sadece `USAGE`) → `/var/log/m2dev-metrics/`, okuma `m2metrics.py --dbstat`; `docs/worklog/2026-10-06-dbstat-collector.md`. Bekleyen: 1 günlük gerçek boyut. **Adım 1a (PR #14):** AsyncSQL baseline'ı, `tools/sql-reliability/` (12 senaryo, 10/10 deterministik) → `docs/worklog/2026-10-06-asyncsql-baseline.md`. Doğrulanan sorunlar (bugünkü kod): retry pratikte çalışmıyor ve kuyruk tıkanabiliyor; 1205/1213/2013 yazmaları kayboluyor; `CountQuery()` kopya kuyruğunu görmüyor ve takılı işler kapanışta kayboluyor; async yoldan sonuç döndüren ifade bağlantıyı bozuyor (bugün çağıran yok). DB yedeği (PR #13) ve sağlık kaydı (PR #12) test VM'de çalışıyor. Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **T-2 / 1.9** sürüm kimliği (etki analizi), sonra **T-1** `syserr.log` yeniden başlatmada korunur → ikisi AsyncSQL düzeltmesi **devreye alınmadan önce** (`docs/roadmap.md`)
2. **AsyncSQL reliability fix** (1a testleri tersine dönmeli)
3. **Şema yönetimi / migration standardı** (sürümlü migration, makinece okunur şema sürümü, InnoDB kuralı)
4. **InnoDB dönüşümü** (migration olarak; `INSERT DELAYED` kaldırma ile birlikte) → yük testiyle kabul
Bağımsız: **T-3** disk boş alanı (dbstat'a küçük ekleme), **T-4** süreç bazında servis durumu (1.7 ile), `db.core` (2026-10-05) incelemesi, **A-12** düzeltmesi (analiz: `docs/engineering/a12-permissions.md`, PR #18; VM izinleri, `perms.py`, `m2build`, servis kullanıcısı ayrı etki analizi ve onayla). Ayrı onay bekleyen: ticaret + geçici trigger testi.

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
