# Durum

Kapsadığı commit: `main` `f151dd2da` (PR #29 merge, 2026-10-11). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline f151dd2da..origin/main -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **PR #29 MERGED** (`f151dd2da`): bahis quest'i + 15 dil dosyası. **A-31/A-32/A-33 BİTTİ.** RB-17 T1/T2/T3/T4/T6 PASS (exact commit
  `10c3d7da8`). **A-17 d29:** runtime PASS, audit "telafi yok" yanlıştı, A-17 kod değişikliği gerekmiyor.
- **Kazanan ödemesi uçtan uca kanıtlanmadı → bahis sistemi production-ready değil** (A-27, release blocker).
- **`main`'de, production kurulumu yok:** A-17 g11 (PR #28), g19 (PR #27), d25, 2a, depo A-23/A-19, T-1/T-2, bahis quest'i (PR #29).

## Sıradaki (her adım ayrı etki analizi + onay)
1. **A-29** lonca kurma paketinde sunucu yetkisi (yüksek) → 2. A-17 kalanı (d16/d27, g6, `QUERY_PLAYER_LOAD` sahipliği) → 3. 2b
→ 4. şema/migration standardı (A-28) → 5. InnoDB
Production/public öncesi: **A-27** ekonomi atomikliği (isim, lonca, bahis AMBIGUOUS/önbellek/`taken_time`; kazanan ödemesi),
**A-35** Türkçe dil dosyası paritesi.
Açık: A-34, A-36, A-37, A-30 (Unverified), A-15, A-16, A-20…A-26, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, release yolu.

## Senden bekleyen kararlar
- A-27'nin başlangıcı; A-18 ölçümü; test VM `/root`'taki listelenmemiş eski dosyalar
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Yerel commit'lenmeyenler: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`, `client/mark/10_0.tga`. GM `admin`.
Test VM: game/db `57eff2d91` (`src=archive`, `dirty=0`) + quest/dil dosyaları = `main` `f151dd2da` runtime'ı. d29 fixture'ları temizlendi
(kanıt `/root/acceptance/gwb-test/cleanup`). `mark_0.tga` yuva 1'de bağlı olmayan test pikselleri. Windows'tan önce `ssh bsd shutdown -p now`.
Production'a kadar tutulan: `/root/acceptance/*`, `/root/a17-quest-prev`, `/root/pr2-acceptance-20261010`, `/root/outage-d29.sh`,
`share/bin/.prev.*`, bütün yedekler.
