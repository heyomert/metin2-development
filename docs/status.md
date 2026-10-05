# Durum

Kapsadığı commit: `fix/anisotropic-filtering` (2026-10-05, PR açık; taban e47ffde2 = PR #10 merge). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline e47ffde2..HEAD -- . ':!docs/status.md'` anizotropi PR'ı dışında bir şey gösteriyorsa eski olabilir; değişikliklere bak, düzelt.

## Şu an
Yazı çizimi Anka2/orijinal görünümde (PR #9 merge). `config.exe` kaynaktan yeniden yazıldı, tam ekranda frekans/MSAA hatası düzeltildi (PR #10 merge). Zemin netliği: DX9 geçişinde kaybolan 4x anizotropi geri geldi (PR açık). Faz 1 güvenlik kalemleri test VM'de tamam (PR #5–#8). Production ☐.

## Sıradaki
1. Monitoring ve log saklama kararı (roadmap 1.4–1.5)
2. DB yedeği + restore testi (1.6), core dump + çökme uyarısı (1.7)

## Senden bekleyen kararlar
- Görsel iyileştirme sırası (öneri): pencere modunda MSAA yok (`EterLib/GrpDevice.cpp:402`) → ağaçlarda en iyi filtre → arayüzde geçersiz `D3DTEXF_NONE` → UI ölçekleme (doğal olmayan çözünürlükte büyütme bulanıklığı)
- Görev listesinde uzun adların yan sütuna taşması (`client/assets/root/interfacemodule.py:1375`, sabit 100 px): düzeltilsin mi
- `vendor/freetype-2.13.3`: build'den çıktı, silinsin mi (öneri: şimdilik kalsın, `tools/font-compare` kullanıyor)
- Client DPI-unaware (%125+ ölçekte bulanık): etki analizi yapılsın mı
- A-1 hile tepki politikası; özellik listesi (F-1 dahil); production sunucu sağlayıcısı (G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil), `client/config/metin2.cfg`'deki kişisel ayarlar

## Ortam
Test VM: `pf` aktif, yönetim şifresi rastgele, `conf/` 750/640. `game` → `/root/build-verify` (K-3 + K-2 B), `db`/`qc` → 4 Ekim derlemesi.
GM hesabı `admin` (şifre repo dışında). Geri dönüş binary'leri: `/root/build-baseline-k3/`, `/root/build-baseline-2026-10-05/`.
Client derlemesi bu makinede `/m:1` ile (paralelde bellek yetmiyor). `Metin2.exe` yönetici yetkisiyle çalışır.
