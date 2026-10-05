# Durum

Kapsadığı commit: `feat/server-metrics` dalı (2026-10-05; taban 0b6d9f00). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline 0b6d9f00..HEAD -- . ':!docs/status.md'` bu daldan başka bir şey gösteriyorsa eski olabilir; değişikliklere bak, düzelt.

## Şu an
Sunucu sağlık kaydı (roadmap 1.5, 1. adım) test VM'de çalışıyor: game süreçleri 10 sn'de bir `log/metrics_<gün>.log` yazıyor, 14 gün saklanıyor. Okuma: `docs/monitoring.md`; özet: `tools/metrics/m2metrics.py`. Test sonuçları ve VM olayı (askıya alma → Aria kaydı bozuldu, kurtarıldı): `docs/worklog/2026-10-05-server-metrics.md`. Önceki işler (font, config.exe, anizotropi, Faz 1 güvenlik) merge'lü. Production ☐.

## Sıradaki
1. Metrik dalı: PR + merge (onayınla)
2. Monitoring 2. adım: süreç RAM/CPU, db süreci, grafik (Prometheus/Grafana → VM'e paket kurulumu, ayrı onay)
3. DB yedeği + restore testi (1.6): bu oturumdaki Aria olayı gerekliliğini gösterdi; core dump + çökme uyarısı (1.7)

## Senden bekleyen kararlar
- `heart_idle` gecikmede fazladan pulse sayıyor (roadmap teknik borç): yük testinden önce etki analizi yapılsın mı
- Görev listesinde uzun adların yan sütuna taşması (`client/assets/root/interfacemodule.py:1375`, sabit 100 px): düzeltilsin mi
- `vendor/freetype-2.13.3`: build'den çıktı, silinsin mi (öneri: şimdilik kalsın, `tools/font-compare` kullanıyor)
- Client DPI-unaware (%125+ ölçekte bulanık): etki analizi yapılsın mı
- A-1 hile tepki politikası; özellik listesi (F-1 dahil); production sunucu sağlayıcısı (G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil), `client/config/metin2.cfg`'deki kişisel ayarlar

## Ortam
Test VM: `pf` aktif, yönetim şifresi rastgele, `conf/` 750/640. `game` → `/root/build-verify` (K-3 + K-2 B + metrics), `db`/`qc` → 4 Ekim derlemesi. Metrik öncesi `game` binary'sinin yedeği VM olayında kayboldu: geri dönüş `METRICS_ENABLE: 0` ya da `main`'den yeniden derleme.
VM'de MariaDB'nin kurtarma öncesi (bozuk hâliyle) soğuk kopyası: `/root/mysql-cold-backup-20261005-postcrash`. Windows'u yeniden başlatmadan önce VM'i kapat (`ssh bsd shutdown -p now`).
GM hesabı `admin` (şifre repo dışında). Client derlemesi `/m:1` ile. `Metin2.exe` yönetici yetkisiyle çalışır.
