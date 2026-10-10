# Durum

Kapsadığı commit: `main` `1e1754b95` (PR #26 merge, 2026-10-10; kod `cf8073d09` ile aynı). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline 1e1754b95..origin/main -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **AsyncSQL 2a** (`docs/engineering/db-step2-asyncsql-fix.md`, `docs/worklog/2026-10-07-asyncsql-2a.md`): **PR #22 merge edildi**
  (`ee0a12a66`); test VM'de exact head ile gerçek client kabul testi PASS (`docs/worklog/2026-10-07-pr22-acceptance.md`), merge
  sonucu runtime ağaçları test edilenle aynı. **Production kurulumu yok.**
- **Regresyon tabanı** (`docs/engineering/regression-baseline.md`, `change-impact.md` §7, `tools/acceptance/outage.sh`): PR #23 merge edildi.
- **Depo (A-23 + A-19)** (`docs/engineering/safebox-activation.md`): **`main`'de** — PR #24 + #25 merge edildi (`cf8073d09`; içerik
  test edilenle aynı). Depo hesap özelliği, 500 yang hesap başına bir kez. Test VM gerçek client kabulleri PASS (A-23 `8f438cc0f`,
  A-19 `71f346d85`). Kabul edilen kalan risk (tasarım §5): satır kalıcı kaldıkça hesap başına en fazla bir ücretsiz aktivasyon;
  oyuncu para kaybı, çift ücret ya da item kaybı değil. **Production kurulumu yok; production'da D' kapısı ayrıca onaylanacak.**
- **T-1, T-2:** test VM tamam (PR #21, #20); production temiz-git release yolu açık → production kurulumu fail-closed.
- **DB standardı:** 1a/1b/1c ve 2a bitti (test VM). Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **A-17 kalan çağıran işi (ana hat)** (isim değiştirme, id 0 lonca, bahis iadesi, SELECT hata/boş, `QUERY_PLAYER_LOAD` sahipliği) → 2. 2b → 3. şema/migration standardı → 4. InnoDB
Production/public açılıştan önce: **A-27** ekonomi atomikliği denetimi (yüksek öncelik, `docs/engineering/economy-atomicity-audit.md`);
production kurulumunda depo D' kapısı (`docs/production-checklist.md`).
Bağımsız: A-15 (channel service dalı), A-16 (diğer ham SQL logları), A-20 (depo protokol temizliği), A-24, A-25, A-26, A-21, A-22, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, db "End of pid", release yolu.

## Senden bekleyen kararlar
- Temizlik: test VM geçici yolları/kanıtlar (aşağıda); A-27'nin başlangıcı
- A-18: kesintide RST hız sınırı ~1 sn takılma (2a öncesinden) — ölçüm/tasarım ne zaman
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`. GM `admin` (şifre repo dışında); `Metin2.exe` yönetici.
Test VM: `pf` aktif, `m2dev_dbstat` açık; game/db = `71f346d85` (`src=archive`; kaynak ağacı `main` `cf8073d09` ile aynı) + A-19 quest'i; önceki çift
`share/bin/.prev.20261010T012003Z.5083` (`8f438cc0f`), eski quest dosyaları `/root/pr2-acceptance-20261010/quest-prev`. Üç
test hesabı (bilgileri repo dışında, secrets). `test` hesabı fixture'ı kabulde kullanıldı (artık satır + item). Windows'tan önce VM'i kapat.
Silme onayı bekleyen: `/root` DB kopyaları (`mysql-cold-*`, `aria-log-corrupt-*`); geçici `/root/{t2a,t2b,build-t2a,build-t2b,t2-tools,1c-tools,src-1c,
t2-e2e,t1-e2e,src-t1,build-t1,src-2a,build-2a,asyncsql-probe,pr22-e2e,c1-check,build-c1,acceptance,outage-tooltest.sh}`, `/root/*f62f10833b44*`, `/root/*8fbec5899676*`, `/root/{outage-e4c5596d.sh,sbprobe-a19,sbprobe-a23,server-src-a23-pr1.tar.gz,src-a23-pr1,build-a23-pr1,build-a23-pr1-*.log,server-src-8f438cc0fcb8.tar.gz,src-8f438cc0fcb8,build-8f438cc0fcb8,build-8f438cc0fcb8.*.log,src-a19-pre,build-a19-pre*,a19-pre.tar.gz,quest-a19-check,quest-a19-check2,probe-compile-a19,src-71f346d85a18,build-71f346d85a18*,a19-71f346d85a18.tar.gz,tools-c8d19d368bab,pr2-acceptance-20261010}`, `/var/tmp/m2sbprobe-*`, `/tmp/outage-test*`, `/var/tmp/{t1-probe,m2inst-rollback-check,eb-run,eb-old,eb-new,m2keep-2a,m2keep-2a-off,m2keep-a17,m2keep-final,m2mix}, /tmp/{d6-old,d6-2a}.txt, /tmp/{sqlline.txt,sz.cpp,sz-old,sz-new,on.txt,off.txt,p1,p2,d7o,d7n,d7os}`;
yerel `stash@{0}` (sadece EOL). Worktree'ler ve merge edilmiş dallar silindi (2026-10-10): GitHub'da yalnız `main`, merge'de dal otomatik siliniyor;
`m2dev-docs-wt` taslağı repo dışında `C:\Users\mertw\.m2dev\docs-wt-draft-2026-10-06.patch`.
