# Durum

Kapsadığı commit: `fix/font-gdi-render` branch'i (2026-10-05, PR açık; taban bc101b2b). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline bc101b2b..HEAD -- . ':!docs/status.md'` sadece font PR'ını göstermiyorsa eski olabilir; değişikliklere bak, düzelt.

## Şu an
Yazı çizimi Anka2/orijinal görünüme döndü (GDI + 1-bit eşik), PR açık. Faz 1 güvenlik kalemleri test VM'de tamam (PR #5–#8). Production ☐.

## Sıradaki
1. `config.exe`'yi kaynaktan yeniden yaz (`client-src/src/Config`); eskisi ayarları yanlış dosyaya yazıyor (roadmap → Teknik borç)
2. Yeni `config.exe` ile font PR'ının çözünürlük / tam ekran / alt-tab testi
3. Monitoring ve log saklama kararı (roadmap 1.4–1.5), DB yedeği + restore (1.6), core dump (1.7)

## Senden bekleyen kararlar
- Font PR'ı: inceleme ve merge
- `vendor/freetype-2.13.3`: build'den çıktı, silinsin mi (öneri: şimdilik kalsın, `tools/font-compare` kullanıyor)
- Client DPI-unaware (%125+ ölçekte bulanık): etki analizi yapılsın mı
- A-1 hile tepki politikası; özellik listesi (F-1 dahil); production sunucu sağlayıcısı (G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil)

## Ortam
Test VM: `pf` aktif, yönetim şifresi rastgele, `conf/` 750/640. `game` → `/root/build-verify` (K-3 + K-2 B), `db`/`qc` → 4 Ekim derlemesi.
GM hesabı `admin` (şifre repo dışında). Geri dönüş binary'leri: `/root/build-baseline-k3/`, `/root/build-baseline-2026-10-05/`.
Client derlemesi bu makinede `/m:1` ile (paralelde bellek yetmiyor).
