# Durum

Kapsadığı commit: PR #23 merge (`579cb2aab`, 2026-10-07) + `fix/a23-safebox-intent` (A-23 PR-1 `8f438cc0f` + docs; yerel, push/PR yok). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline 579cb2aab..origin/main -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **AsyncSQL 2a** (`docs/engineering/db-step2-asyncsql-fix.md`, `docs/worklog/2026-10-07-asyncsql-2a.md`): **PR #22 merge edildi**
  (`ee0a12a66`); test VM'de exact head ile gerçek client kabul testi PASS (`docs/worklog/2026-10-07-pr22-acceptance.md`), merge
  sonucu runtime ağaçları test edilenle aynı. **Production kurulumu yok.**
- **Regresyon tabanı** (`docs/engineering/regression-baseline.md`, `change-impact.md` §7, `tools/acceptance/outage.sh`): PR #23 merge edildi.
- **Depo (A-19/A-23)** (`docs/engineering/safebox-activation.md`): karar: depo hesap özelliği, 500 yang hesap başına bir kez.
  PR-1 (A-23 açma intent'i, `8f438cc0f`) test VM gerçek client kabulü **PASS** (2026-10-08); tek başına merge edilmez (PR-2 üstünde).
- **T-1, T-2:** test VM tamam (PR #21, #20); production temiz-git release yolu açık → production kurulumu fail-closed.
- **DB standardı:** 1a/1b/1c ve 2a bitti (test VM). Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
0. Depo: PR-1 kabulü → PR-2 (hesap aktivasyonu + blokajlı ödeme) → birleşik kabul → merge onayı; production'da D' kapısı
1. A-17 kalan çağıran işi (isim değiştirme, id 0 lonca, bahis iadesi, SELECT hata/boş, `QUERY_PLAYER_LOAD` sahipliği) → 2. 2b → 3. şema/migration standardı → 4. InnoDB
Bağımsız: A-15 (channel service dalı), A-16 (diğer ham SQL logları), A-20 (depo protokol temizliği), A-24, A-25, A-21, A-22, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, db "End of pid", release yolu.

## Senden bekleyen kararlar
- Depo PR-2 (hesap aktivasyonu + blokajlı ödeme) implementasyon onayı
- A-18: kesintide RST hız sınırı ~1 sn takılma (2a öncesinden) — ölçüm/tasarım ne zaman
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`. GM `admin` (şifre repo dışında); `Metin2.exe` yönetici.
Test VM: `pf` aktif, `m2dev_dbstat` açık; game/db = `8f438cc0f` (`src=archive`, PR-1 dalı, `main` değil); önceki çift `share/bin/.prev.20261007T201137Z.26764`
(`88912c7ab`). Windows'tan önce VM'i kapat.
Silme onayı bekleyen: `/root` DB kopyaları (`mysql-cold-*`, `aria-log-corrupt-*`); geçici `/root/{t2a,t2b,build-t2a,build-t2b,t2-tools,1c-tools,src-1c,
t2-e2e,t1-e2e,src-t1,build-t1,src-2a,build-2a,asyncsql-probe,pr22-e2e,c1-check,build-c1,acceptance,outage-tooltest.sh}`, `/root/*f62f10833b44*`, `/root/*8fbec5899676*`, `/root/{outage-e4c5596d.sh,sbprobe-a19,sbprobe-a23,server-src-a23-pr1.tar.gz,src-a23-pr1,build-a23-pr1,build-a23-pr1-*.log,server-src-8f438cc0fcb8.tar.gz,src-8f438cc0fcb8,build-8f438cc0fcb8,build-8f438cc0fcb8.*.log}`, `/var/tmp/m2sbprobe-*`, `/tmp/outage-test*`, `/var/tmp/{t1-probe,m2inst-rollback-check,eb-run,eb-old,eb-new,m2keep-2a,m2keep-2a-off,m2keep-a17,m2keep-final,m2mix}, /tmp/{d6-old,d6-2a}.txt, /tmp/{sqlline.txt,sz.cpp,sz-old,sz-new,on.txt,off.txt,p1,p2,d7o,d7n,d7os}`;
depo fixture'ı (satırsız + item) bilerek korunuyor; yerel `m2dev-a23-wt` (PR-1 dalı), `m2dev-docs-wt`, `m2dev-a12fix`, `m2dev-main-wt`, stash, `feat/build-identity`, `feat/syserr-preserve` ve `feat/asyncsql-2a` dalları.
