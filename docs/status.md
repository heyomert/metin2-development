# Durum

Kapsadığı commit: DB adım 1b PR'ı (2026-10-06; taban 32159c3c). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline 32159c3c..HEAD -- . ':!docs/status.md'` 1b PR'ı dışında bir şey gösteriyorsa eski olabilir; değişikliklere bak, düzelt.

## Şu an
DB standardı çalışması (plan: `docs/engineering/db-standard.md`). **Adım 1b bitti:** bağımsız, salt okunur MariaDB/OS toplayıcısı `m2dev-dbstat` test VM'de servis olarak çalışıyor (`m2stat`, sadece `USAGE`) → `/var/log/m2dev-metrics/`, okuma `m2metrics.py --dbstat`; `docs/worklog/2026-10-06-dbstat-collector.md`. Bekleyen: 1 günlük gerçek boyut. **Adım 1a (PR #14):** AsyncSQL baseline'ı, `tools/sql-reliability/` (12 senaryo, 10/10 deterministik) → `docs/worklog/2026-10-06-asyncsql-baseline.md`. Doğrulanan sorunlar (bugünkü kod): retry pratikte çalışmıyor ve kuyruk tıkanabiliyor; 1205/1213/2013 yazmaları kayboluyor; `CountQuery()` kopya kuyruğunu görmüyor ve takılı işler kapanışta kayboluyor; async yoldan sonuç döndüren ifade bağlantıyı bozuyor (bugün çağıran yok). DB yedeği (PR #13) ve sağlık kaydı (PR #12) test VM'de çalışıyor. Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **1c** AsyncSQL/db gözlem sayaçları (metrics bileşeni seçenekleri karşılaştırılacak; davranış 1a ile aynı kalmalı; tablo kilidi süresi burada)
2. **AsyncSQL reliability fix** (1a testleri tersine dönmeli)
3. **Şema yönetimi / migration standardı** (sürümlü migration, InnoDB kuralı)
4. **InnoDB dönüşümü** (migration olarak; `INSERT DELAYED` kaldırma ile birlikte) → yük testiyle kabul
Ayrı onay bekleyen: ticaret + geçici trigger testi (değer kaybı mı, dupe mı).

## Senden bekleyen kararlar
- `heart_idle` gecikmede fazladan pulse sayıyor (teknik borç): yük testinden önce etki analizi yapılsın mı
- Dakika hassasiyetinde geri dönüş (binlog + PITR) gerekli mi
- Görev listesinde uzun adların taşması (`client/assets/root/interfacemodule.py:1375`); `vendor/freetype-2.13.3` silinsin mi; client DPI
- A-1 hile tepki politikası; özellik listesi (F-1 dahil); production sunucu sağlayıcısı (G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil), `client/config/metin2.cfg`'deki kişisel ayarlar

## Ortam
Test VM: `pf` aktif, `conf/` 750/640. `m2dev_dbstat` servisi açık; geliştirme dizini `/root/dbstat-dev`. `game` → `/root/build-verify` (K-3 + K-2 B + metrics), `db`/`qc` → 4 Ekim derlemesi. Windows'u yeniden başlatmadan önce VM'i kapat (`ssh bsd shutdown -p now`).
VM `/root` geçici güvenlik kopyaları (silmek için sor): `mysql-cold-backup-20261005-postcrash`, `aria-log-corrupt-20261005`, `mysql-cold-pre-drill-20261005`.
GM hesabı `admin` (şifre repo dışında). Client derlemesi `/m:1` ile. `Metin2.exe` yönetici yetkisiyle çalışır.
