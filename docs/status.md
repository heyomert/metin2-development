# Durum

Kapsadığı commit: `fix/guild-war-bet-quest` (2026-10-11; main = `36d70049c`). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline origin/fix/guild-war-bet-quest..origin/main -- . ':!docs/status.md'` bir şey gösteriyorsa eski olabilir.

## Şu an
- **A-17 d29 / A-31…A-33 bahis quest'i** (PR #29, `docs/worklog/2026-10-11-guild-war-bet-quest.md`): d29 "telafi yok" yanlıştı, A-17 kod
  değişikliği gerekmiyor. Quest + 15 dil dosyası düzeltmesi exact commit ile **RB-17 T1/T2/T3/T4/T6 PASS**; **merge onay bekliyor**.
  Kazanan ödemesi uçtan uca kanıtlanmadı → bahis sistemi production-ready değil (A-27, release kabulü).
- **Bitenler (`main`, production kurulumu yok):** A-17 g11 (PR #28), g19 (PR #27), d25, 2a, depo A-23/A-19, T-1/T-2.

## Sıradaki (her adım ayrı etki analizi + onay)
1. PR #29 merge (d29 kapanır) → 2. **A-29** lonca kurma paketinde sunucu yetkisi (yüksek) → 3. A-17 kalanı
(d16/d27, g6, `QUERY_PLAYER_LOAD` sahipliği) → 4. 2b → 5. şema/migration standardı (A-28) → 6. InnoDB
Production/public öncesi: **A-27** ekonomi atomikliği (isim, lonca, bahis; kazanan ödemesi), **A-35** Türkçe dil dosyası paritesi.
Bağımsız: A-34, A-36, A-37, A-30 (Unverified), A-15, A-16, A-20…A-26, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, release yolu.

## Senden bekleyen kararlar
- A-27'nin başlangıcı; A-18 ölçümü; test VM `/root`'taki listelenmemiş eski dosyalar
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Yerel commit'lenmeyenler: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`, `client/mark/10_0.tga`. GM `admin`.
Test VM: game/db `57eff2d91` (= `main` runtime); d29 fixture'ları: hesap 7 `d29test`/D29Char, gmlist mID 4, lonca 5 `BetBeta`,
rezervasyon 1-3. `mark_0.tga` yuva 1'de bağlı olmayan test pikselleri. Windows'tan önce `ssh bsd shutdown -p now`.
Production'a kadar tutulan: `/root/acceptance/*`, `/root/a17-quest-prev`, `/root/pr2-acceptance-20261010`, `share/bin/.prev.*`, yedekler.
