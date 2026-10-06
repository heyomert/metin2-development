# Durum

Kapsadığı commit: PR #21 merge (8fbec589) + T-1 uçtan uca testi (2026-10-07). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline 8fbec589..HEAD -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **T-2 / 1.9:** test VM tamam (PR #20); **production temiz-git release yolu açık** → production kurulumu fail-closed.
- **T-1:** test VM tamam (PR #21; uçtan uca: 12/12 arşiv birebir, `kill -9` sonrası "End of pid" yok, giriş, SQL hata 0).
  Bulgular (roadmap): db "End of pid" yazmıyor, A-14 sınırsız `syserr`, `CHECKPOINT` satırı kayboluyor.
- **DB standardı** (`docs/engineering/db-standard.md`): 1a/1b/1c bitti, S12 düzeltme adımında; yedek + sağlık kaydı çalışıyor. Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **AsyncSQL reliability fix** (1a testleri tersine dönmeli; S12 dahil) → 2. **Şema/migration standardı** → 3. **InnoDB dönüşümü**
Bağımsız: T-3, T-4, `db.core`, A-14 salt okuma, A-12, CHECKPOINT, db "End of pid", `server-src` `eol` kuralı, release yolu, ticaret testi.

## Senden bekleyen kararlar
- `heart_idle` fazladan pulse (yük testinden önce analiz?); binlog + PITR gerekli mi; uzun ad taşması; freetype silinsin mi; DPI
- A-1 hile tepki politikası; özellik listesi (F-1); production sunucu sağlayıcısı (G-3, G-4)

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`. GM `admin` (şifre repo dışında); `Metin2.exe` yönetici.
Test VM: `pf` aktif, `m2dev_dbstat` açık; game/db = `8fbec589` (`src=archive`, `share/bin/BUILD`); önceki T-2 çifti
`share/bin/.prev.20261006T212640Z.59687/` (kimlikli → installer ile geri dönüş). Windows'tan önce VM'i kapat.
Silme onayı bekleyen: `/root` DB kopyaları (`mysql-cold-*`, `aria-log-corrupt-*`); geçici `/root/{t2a,t2b,build-t2a,build-t2b,t2-tools,
1c-tools,src-1c,t2-e2e,t1-e2e,src-t1,build-t1}`, `/root/*f62f10833b44*`, `/root/*8fbec5899676*`, `/var/tmp/{t1-probe,m2inst-rollback-check}`;
yerel `m2dev-docs-wt`, `m2dev-a12fix`, stash, `feat/build-identity` ve `feat/syserr-preserve` dalları.
