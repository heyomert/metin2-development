# Durum

Kapsadığı commit: PR #28 (`fix/a17-g11-guild-create`, kod `57eff2d91`, 2026-10-11; main = `be7907d0e`). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline origin/fix/a17-g11-guild-create..origin/main -- . ':!docs/status.md'` bir şey gösteriyorsa eski olabilir.

## Şu an
- **A-17 g11 lonca kurma** (`docs/worklog/2026-10-11-guild-create-a17.md`): düzeltme öncesi hayalet lonca 0, `UNIQUE pid` ile kalıcı
  üyelik kilidi ve sonraki kurulumda ücret alınıp lider eklenmemesi test VM'de kanıtlandı. Düzeltme (başarısız INSERT'te yan etki
  yok + db yalnız `guild_id = 0` reddi): **PR #28 açık**, birim 6/6, test VM önce/sonra R1/R2/R3 + RB-14 PASS, exact commit final
  gate PASS. **Merge onay bekliyor; production kurulumu yok.**
- **A-17 g19 isim değiştirme:** PR #27 merge edildi (`be7907d0e`). Önceki işler `main`'de; hiçbirinin production kurulumu yok.

## Sıradaki (her adım ayrı etki analizi + onay)
1. PR #28 merge → 2. A-29 (lonca kurma paketinde sunucu yetkisi, yüksek) → 3. A-17'nin kalanı (d29 bahis, g6 vb.,
`QUERY_PLAYER_LOAD` sahipliği) → 4. 2b → 5. şema/migration standardı (A-28) → 6. InnoDB
Production/public açılıştan önce: **A-27** ekonomi atomikliği (isim değiştirme çökmesi, lonca ücreti/üyelik dahil).
Bağımsız: A-30 (Unverified), A-15, A-16, A-20, A-24, A-25, A-26, A-21, A-22, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, release yolu.

## Senden bekleyen kararlar
- PR #28 merge; merge sonrası g11 test temizliği (`g11test`/G11Char, gmlist `mID=3`, lonca G11Real id 4, işaret yuvası 1)
- A-27'nin başlangıcı; A-18 ölçümü; test VM `/root`'taki listelenmemiş eski dosyalar
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`, `client/mark/10_0.tga`. GM `admin`; `Metin2.exe` yönetici.
Test VM: **game/db = `57eff2d91` (PR #28, `src=archive`, `dirty=0`, policy test-vm)**; önceki çift
`share/bin/.prev.20261010T233210Z.41411` (g11 çalışma ağacı, `dirty=1`), ondan önceki `.prev.20261010T231034Z.21232` (`174ef0566`). Kanıt `/root/acceptance/{g11-repro,g11-after,a17-*}`, `/root/a17-quest-prev`.
`share/mark/mark_0.tga` yuva 1-2'de testten kalan varsayılan işaret (öncesi kopyası yoktu). `pf` aktif, `m2dev_dbstat` açık.
Windows'tan önce VM'i kapat (`ssh bsd shutdown -p now`). Production'a kadar tutulan: `/root/pr2-acceptance-20261010`, `/root/acceptance/g11-*`,
`share/bin/.prev.*`, `consistent` yedekler.
