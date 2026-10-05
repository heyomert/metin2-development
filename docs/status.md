# Durum

Kapsadığı commit: PR #13 merge'ü (2026-10-06; taban 82e3ef8c). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline 82e3ef8c..HEAD -- . ':!docs/status.md'` PR #13 dışında bir şey gösteriyorsa eski olabilir; değişikliklere bak, düzelt.

## Şu an
DB yedeği (roadmap 1.6) test VM'de çalışıyor: saatlik şifreli döküm (cron), günlük çekme + geri yükleme testi (Windows Görev Zamanlayıcı `m2dev-backup-daily`), tatbikat oyunda doğrulandı (PR #13 merge) → `docs/backup.md`, `docs/worklog/2026-10-05-db-backup.md`. Sağlık kaydı (1.5 adım 1, PR #12) çalışıyor → `docs/monitoring.md`. Production ☐.

## Sıradaki
1. **Aria → InnoDB etki analizi** (yüksek risk: analiz → onay → kod). Gerekçe ölçüldü: yedek kilidi 3,5 milyon item'da 8 sn, Aria tablo kilidi, çöküşte Aria kaydı. Production'dan önce şart
2. Yedeğin birkaç gün gözetimsiz çalışmasını `status-backup-hot` / `status-daily` ile izle
3. Monitoring 2. adım (RAM/CPU, yedek durumu uyarısı, grafik → VM'e paket, ayrı onay); core dump + çökme uyarısı (1.7)

## Senden bekleyen kararlar
- **Özel yedek anahtarının ikinci kopyası** (`C:\Users\mertw\.m2dev\secrets\m2dev-backup.agekey`): sende, ayrı bir yerde
- `heart_idle` gecikmede fazladan pulse sayıyor (teknik borç): yük testinden önce etki analizi yapılsın mı
- Dakika hassasiyetinde geri dönüş (binlog + PITR) gerekli mi
- Görev listesinde uzun adların taşması (`client/assets/root/interfacemodule.py:1375`); `vendor/freetype-2.13.3` silinsin mi; client DPI
- A-1 hile tepki politikası; özellik listesi (F-1 dahil); production sunucu sağlayıcısı (G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil), `client/config/metin2.cfg`'deki kişisel ayarlar

## Ortam
Test VM: `pf` aktif, `conf/` 750/640. `game` → `/root/build-verify` (K-3 + K-2 B + metrics), `db`/`qc` → 4 Ekim derlemesi. Windows'u yeniden başlatmadan önce VM'i kapat (`ssh bsd shutdown -p now`).
VM `/root` geçici güvenlik kopyaları (silmek için sor): `mysql-cold-backup-20261005-postcrash`, `aria-log-corrupt-20261005`, `mysql-cold-pre-drill-20261005`.
GM hesabı `admin` (şifre repo dışında). Client derlemesi `/m:1` ile. `Metin2.exe` yönetici yetkisiyle çalışır.
