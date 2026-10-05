# Durum

Kapsadığı commit: 28d62e3d (2026-10-05). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline 28d62e3d..HEAD -- . ':!docs/status.md'` boş değilse eski olabilir; değişikliklere bak, düzelt.

## Şu an
Faz 1 — güvenlik kalemleri test VM'de tamam: K-1 P2P firewall (PR #5), K-3 yönetim kanalı (PR #6, #7), K-2 A+B (PR #8). Production ☐.

## Sıradaki
1. Monitoring ve log saklama kararı (roadmap 1.4–1.5; rotasyon kodda var, 7 gün, arşivler `644`)
2. DB yedeği + restore testi (roadmap 1.6)
3. Core dump + çökme uyarısı (roadmap 1.7; production binary'lerinde debug bilgisi kararı dahil)

## Senden bekleyen kararlar
- A-1: hile tespitleri (zaman / saldırı hızı / kombo) sadece logluyor; etkinleştirme politikası
- Özellik listesi (F-1 çoklu client sınırı dahil), kademe kademe verilecek
- Production sunucu sağlayıcısı: FreeBSD, DDoS koruması, Türkiye gecikmesi (roadmap G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil)

## Ortam
Test VM: `pf` aktif, yönetim şifresi rastgele, `conf/` 750/640. `game` → `/root/build-verify` (K-3 + K-2 B), `db`/`qc` → 4 Ekim derlemesi.
GM hesabı `admin` (şifre repo dışında). Açık PR: yok. Geri dönüş binary'leri: `/root/build-baseline-k3/`, `/root/build-baseline-2026-10-05/`.
