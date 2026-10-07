# Durum

Kapsadığı commit: PR #22 merge (`ee0a12a66`, 2026-10-07) + regresyon tabanı PR'ı (`docs/regression-baseline`). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline ee0a12a66..origin/main -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **AsyncSQL 2a** (`docs/engineering/db-step2-asyncsql-fix.md`, `docs/worklog/2026-10-07-asyncsql-2a.md`): **PR #22 merge edildi**
  (`ee0a12a66`); test VM'de exact head ile gerçek client kabul testi PASS (`docs/worklog/2026-10-07-pr22-acceptance.md`), merge
  sonucu runtime ağaçları test edilenle aynı. **Production kurulumu yok.**
- **Regresyon tabanı** (`docs/engineering/regression-baseline.md`, `change-impact.md` §7, `tools/acceptance/outage.sh`): ayrı PR, merge onayı bekliyor.
- **T-1, T-2:** test VM tamam (PR #21, #20); production temiz-git release yolu açık → production kurulumu fail-closed.
- **DB standardı:** 1a/1b/1c ve 2a bitti (test VM). Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. A-17 kalan çağıran işi (isim değiştirme, id 0 lonca, bahis iadesi, SELECT hata/boş, `QUERY_PLAYER_LOAD` sahipliği) → 2. 2b → 3. şema/migration standardı → 4. InnoDB
Bağımsız: A-15 (channel service dalı), A-16 (diğer ham SQL logları), A-19/A-20 (depo), A-21, A-22, T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, db "End of pid", release yolu.

## Senden bekleyen kararlar
- Regresyon tabanı PR'ının merge onayı
- A-18: kesintide RST hız sınırı ~1 sn takılma (2a öncesinden) — ölçüm/tasarım ne zaman
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`. GM `admin` (şifre repo dışında); `Metin2.exe` yönetici.
Test VM: `pf` aktif, `m2dev_dbstat` açık; game/db = `88912c7ab` (`src=archive`, runtime = `main`); önceki çift `share/bin/.prev.20261007T142159Z.95275`
(`8fbec589`). Windows'tan önce VM'i kapat.
Silme onayı bekleyen: `/root` DB kopyaları (`mysql-cold-*`, `aria-log-corrupt-*`); geçici `/root/{t2a,t2b,build-t2a,build-t2b,t2-tools,1c-tools,src-1c,
t2-e2e,t1-e2e,src-t1,build-t1,src-2a,build-2a,asyncsql-probe,pr22-e2e,c1-check,build-c1,acceptance,outage-tooltest.sh}`, `/root/*f62f10833b44*`, `/root/*8fbec5899676*`, `/var/tmp/{t1-probe,m2inst-rollback-check,eb-run,eb-old,eb-new,m2keep-2a,m2keep-2a-off,m2keep-a17,m2keep-final,m2mix}, /tmp/{d6-old,d6-2a}.txt, /tmp/{sqlline.txt,sz.cpp,sz-old,sz-new,on.txt,off.txt,p1,p2,d7o,d7n,d7os}`;
yerel `m2dev-docs-wt`, `m2dev-a12fix`, `m2dev-main-wt`, stash, `feat/build-identity`, `feat/syserr-preserve` ve `feat/asyncsql-2a` dalları.
