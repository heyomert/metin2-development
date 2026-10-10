# Durum

Kapsadığı commit: `main` `49a4ebea8` (PR #28 merge, 2026-10-11). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline 49a4ebea8..origin/main -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **A-17 g11 lonca kurma: BİTTİ** — PR #28 merge edildi (`49a4ebea8`; kod `57eff2d91`). Başarısız INSERT'te yan etki yok + db yalnız
  `guild_id = 0` reddi. Test VM önce/sonra + exact commit final gate PASS (`docs/worklog/2026-10-11-guild-create-a17.md`). Açık
  bırakılanlar: d11 ve lonca ücreti/üyelik atomikliği (A-27), sıfır olmayan olmayan-lonca id savunması (hardening adayı).
- **A-17 g19 isim değiştirme:** PR #27 (`be7907d0e`). Önceki işler `main`'de; hiçbirinin production kurulumu yok.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **A-17'nin sıradaki maddesi: d29** lonca savaşı bahsi (yang quest'te önceden düşülüyor, INSERT başarısızsa iade yok;
   audit öncelik sırası d25 ✓, g19 ✓, d29, d16/d27, g6/g18) → 2. **A-29** lonca kurma paketinde sunucu yetkisi (yüksek)
→ 3. A-17 kalanı (d16/d27, g6, `QUERY_PLAYER_LOAD` sahipliği) → 4. 2b → 5. şema/migration standardı (A-28) → 6. InnoDB
Production/public açılıştan önce: **A-27** ekonomi atomikliği (isim değiştirme çökmesi, lonca ücreti/üyelik dahil).
Bağımsız: A-30 (Unverified), A-15, A-16, A-20, A-24, A-25, A-26, A-21, A-22, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, release yolu.

## Senden bekleyen kararlar
- A-27'nin başlangıcı; A-18 ölçümü; test VM `/root`'taki listelenmemiş eski dosyalar
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`, `client/mark/10_0.tga`. GM `admin`; `Metin2.exe` yönetici.
Test VM: game/db = `57eff2d91` (`src=archive`, `dirty=0`; runtime ağacı `main` `49a4ebea8` ile aynı). g11 test kalıntıları (hesap 6,
pid 13, lonca 4, gmlist mID 3) onaylı temizlikle kaldırılıyor. `share/mark/mark_0.tga` yuva 1'de bağlı olmayan test pikselleri (öncesi
kopyası yok, belgeli). `pf` aktif, `m2dev_dbstat` açık. Windows'tan önce VM'i kapat (`ssh bsd shutdown -p now`).
Production'a kadar tutulan: `/root/acceptance/{g11-repro,g11-after,a17-*}`, `/root/a17-quest-prev`, `/root/pr2-acceptance-20261010`,
`share/bin/.prev.*`, bütün `consistent` yedekler.
