# Durum

Kapsadığı commit: PR #27 (`fix/a17-change-name`, 2026-10-11; main = `32b9ac6a7`). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline origin/fix/a17-change-name..origin/main -- . ':!docs/status.md'` bir şey gösteriyorsa eski olabilir.

## Şu an
- **A-17 isim değiştirme (g19 + g18 DB hatası kısmı)** (`docs/worklog/2026-10-10-change-name-a17.md`): PR #27 hazır. Test VM'de
  önce item kaybı uçtan uca kanıtlandı; yeni binary ile kesinti (A1), UPDATE reddi (A1b, geçici trigger kaldırıldı), kullanılan ad
  (A3) ve başarı (A2) PASS; RB-01/06/10/14 PASS. **Merge onay bekliyor; production kurulumu yok.**
- **Önceki işler `main`'de, production kurulumu yok:** AsyncSQL 2a (PR #22), regresyon tabanı (#23), depo A-23 + A-19 (#24, #25;
  production'da D' kapısı ayrıca onaylanacak), T-1/T-2 (#21, #20; production temiz-git release yolu açık → fail-closed).

## Sıradaki (her adım ayrı etki analizi + onay)
1. PR #27 merge → 2. A-17'nin kalanı (id 0 lonca g11, bahis iadesi d29, SELECT hata/boş g6 vb., `QUERY_PLAYER_LOAD` sahipliği)
→ 3. 2b → 4. şema/migration standardı (A-28 ad benzersizliği dahil) → 5. InnoDB
Production/public açılıştan önce: **A-27** ekonomi atomikliği (isim değiştirmede çökme sonrası bir ücretsiz değişiklik adayı dahil).
Bağımsız: A-15, A-16, A-20, A-24, A-25, A-26, A-21, A-22, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, db "End of pid", release yolu.

## Senden bekleyen kararlar
- PR #27 merge onayı; A-27'nin başlangıcı; test VM `/root`'taki listelenmemiş eski dosyalar
- A-18: kesintide RST hız sınırı ~1 sn takılma — ölçüm/tasarım ne zaman
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`, `client/mark/10_0.tga`. GM `admin`; `Metin2.exe` yönetici.
Test VM: **game/db = `174ef0566` (PR #27, `src=archive`) + yeni `change_name.quest`**; önceki çift `share/bin/.prev.20261010T205007Z.13132`
(`71f346d85`), eski quest dosyaları `/root/a17-quest-prev`, kanıt `/root/acceptance/a17-*`. Test karakteri `pr2a`/SbA4y (seviye 35,
`next_time` dolu). `pf` aktif, `m2dev_dbstat` açık. Windows'tan önce VM'i kapat (`ssh bsd shutdown -p now`).
Production'a kadar tutulan: `/root/pr2-acceptance-20261010`, `share/bin/.prev.*`. Yerel `stash@{0}` (sadece EOL).
