# Durum

Kapsadığı commit: 804415e7 (2026-10-05). **Üzerine yaz, ekleme yapma**; biten madde silinir (geçmiş: git, `docs/worklog/`). En fazla ~25 satır.
Güncel mi? `git log --oneline 804415e7..HEAD -- . ':!docs/status.md'` boş değilse eski olabilir; değişikliklere bak, düzelt.

## Şu an
Faz 1 — güvenlik temeli (`docs/roadmap.md`). Test VM'de: K-1 P2P firewall ✅ (PR #5), K-3 config adımı ✅ (PR #6), server derleme süreci doğrulandı (A-9). Production ☐.

## Sıradaki
1. K-3 kod adımı: şifrenin log'a düşmesini engelle + A-11 (etki analiziyle; ilk binary devreye alma, DB bağlantısı testi dahil)
2. K-2: şifre saklama yöntemi (yüksek risk, önce etki analizi)
3. Log rotasyonu ve monitoring (roadmap 1.4–1.5)

## Senden bekleyen kararlar
- A-1: hile tespitleri (zaman / saldırı hızı / kombo) sadece logluyor; etkinleştirme politikası
- Özellik listesi (F-1 çoklu client sınırı dahil), kademe kademe verilecek
- Production sunucu sağlayıcısı: FreeBSD, DDoS koruması, Türkiye gecikmesi (roadmap G-3, G-4)

## Yerelde bilerek commit'lenmeyenler
`client/assets/root/serverinfo.py` (VM IP'si), `client/config/locale.cfg` (dil)

## Ortam
Test VM: `pf` aktif, `service m2dev` açılışta başlıyor, yönetim şifresi rastgele (repo dışında), `conf/` 750/640.
GM hesabı `admin` (şifre repo dışında). Açık PR: yok.
