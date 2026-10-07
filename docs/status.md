# Durum

Kapsadığı commit: PR #22 (`feat/asyncsql-2a`, 2026-10-07; main = 30adca70). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline origin/feat/asyncsql-2a..origin/main -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **AsyncSQL 2a** (`docs/engineering/db-step2-asyncsql-fix.md`, `docs/worklog/2026-10-07-asyncsql-2a.md`): kodlandı, test edildi;
  merge öncesi kapılar: `DirectQuery` çağıran denetimi + A-17 merkezi düzeltme + `ReserveWar` (`docs/engineering/
  db-step2a-directquery-audit.md`), `CLIENT_MULTI_STATEMENTS` kapalı, defter taşması, gerçek defterle `m2metrics`, tam regresyon. **PR #22 açık; merge, VM kurulumu ve servis yeniden başlatma yapılmadı** — onay bekleniyor.
- **T-1, T-2:** test VM tamam (PR #21, #20); production temiz-git release yolu açık → production kurulumu fail-closed.
- **DB standardı:** 1a/1b/1c bitti; 2a kabul aşamasında. Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. PR #22 merge → test VM'de kurulum + uçtan uca (giriş, karakter oluşturma, kapanış, DB kesintisi) → 2. 2b → 3. şema/migration standardı → 4. InnoDB
Bağımsız: A-15 (channel service dalı), A-16 (diğer ham SQL logları), T-3, T-4, `db.core`, A-14, A-12, CHECKPOINT, db "End of pid", release yolu.

## Senden bekleyen kararlar
- PR #22 merge onayı; ardından VM kurulumu (ayrı onay). Kalan A-17 çağıran işi ayrı PR
- A-18: kesintide RST hız sınırı ~1 sn takılma (2a öncesinden) — ölçüm/tasarım ne zaman
- `heart_idle` fazladan pulse; binlog + PITR; uzun ad taşması; freetype; DPI; A-1 hile politikası; F-1; G-3, G-4

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`. GM `admin` (şifre repo dışında); `Metin2.exe` yönetici.
Test VM: `pf` aktif, `m2dev_dbstat` açık; game/db = `8fbec589` (`src=archive`); 2a derlemesi `/root/build-2a` (kurulmadı). Windows'tan önce VM'i kapat.
Silme onayı bekleyen: `/root` DB kopyaları (`mysql-cold-*`, `aria-log-corrupt-*`); geçici `/root/{t2a,t2b,build-t2a,build-t2b,t2-tools,1c-tools,src-1c,
t2-e2e,t1-e2e,src-t1,build-t1,src-2a,build-2a,asyncsql-probe}`, `/root/*f62f10833b44*`, `/root/*8fbec5899676*`, `/var/tmp/{t1-probe,m2inst-rollback-check,eb-run,eb-old,eb-new,m2keep-2a,m2keep-2a-off,m2keep-a17,m2keep-final,m2mix}, /tmp/{d6-old,d6-2a}.txt, /tmp/{sqlline.txt,sz.cpp,sz-old,sz-new,on.txt,off.txt,p1,p2,d7o,d7n,d7os}`;
yerel `m2dev-docs-wt`, `m2dev-a12fix`, stash, `feat/build-identity` ve `feat/syserr-preserve` dalları.
