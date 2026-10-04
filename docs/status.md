# Durum

Kapsadığı commit: db858d34 (2026-10-05). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline db858d34..HEAD -- . ':!docs/status.md'` boş değilse eski olabilir; değişikliklere bak, düzelt.

## Şu an
Faz 1 — güvenlik temeli ve gözlemlenebilirlik (`docs/roadmap.md`). K-1 (P2P firewall): test VM ✅ (PR #5), production ☐.

## Sıradaki
1. K-3: yönetim kanalı şifresi ve log'a yazılması
2. K-2: şifre saklama yöntemi (yüksek risk, önce etki analizi)
3. Log rotasyonu ve monitoring (roadmap 1.4–1.5)

## Senden bekleyen kararlar
- A-1: hile tespitleri (zaman / saldırı hızı / kombo) sadece logluyor; etkinleştirme politikası
- Özellik listesi (F-1 çoklu client sınırı dahil), kademe kademe verilecek
- Production sunucu sağlayıcısı: FreeBSD, DDoS koruması, Türkiye gecikmesi (roadmap G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil)

## Ortam
Test VM: `pf` aktif (`deploy/freebsd/pf.conf`), `service m2dev` açılışta başlıyor (4 çekirdek + CH99, P2P mesh tam).
GM hesabı `admin` (şifre repo dışında). Açık PR: yok.
