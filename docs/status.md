# Durum

Kapsadığı commit: PR #20 merge (f62f1083) + T-2 uçtan uca testi (2026-10-06). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline f62f1083..HEAD -- . ':!docs/status.md'` bu güncelleme dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **T-2 / 1.9:** test VM tamam (PR #20, uçtan uca PASS); **production temiz-git release yolu açık** → production kurulumu fail-closed.
  Yeni bulgular (roadmap): db "End of pid" yazmıyor, A-14 sınırsız `syserr`, `CHECKPOINT` satırı kayboluyor.
- **DB standardı** (`docs/engineering/db-standard.md`): 1a/1b/1c bitti, S12 düzeltme adımında; yedek + sağlık kaydı çalışıyor. Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **T-1** (onaylı, C + E): eski `syserr.log` → `log/syserr_*.log`, N=30, `flush_on(err)` (200 bin satır ölçümüne bağlı) → PR
2. **AsyncSQL reliability fix** (1a testleri tersine dönmeli) → 3. **Şema/migration standardı** → 4. **InnoDB dönüşümü**
Bağımsız: T-3 disk boş alanı, T-4 süreç bazında servis durumu, `db.core` incelemesi, A-14 salt okuma analizi, A-12 düzeltmesi,
production temiz-git release yolu, tekrar üretilebilir derleme. Ayrı onay bekleyen: ticaret + geçici trigger testi.

## Senden bekleyen kararlar
- `heart_idle` fazladan pulse (yük testinden önce analiz?); binlog + PITR gerekli mi; uzun ad taşması; freetype silinsin mi; DPI
- A-1 hile tepki politikası; özellik listesi (F-1); production sunucu sağlayıcısı (G-3, G-4)

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`. GM `admin` (şifre repo dışında); `Metin2.exe` yönetici.
Test VM: `pf` aktif, `m2dev_dbstat` açık; game/db = `f62f10833` (`src=archive`, `share/bin/BUILD`); önceki 1c çifti
`share/bin/.prev.20261006T202254Z.51584/` (kimliksiz → elle geri dönüş, `docs/build-and-run.md`). Windows'tan önce VM'i kapat.
Silme onayı bekleyen: `/root` DB kopyaları (`mysql-cold-*`, `aria-log-corrupt-*`); geçici `t2a`, `t2b`, `build-t2a/b`, `t2-tools`,
`1c-tools`, `src-1c`, `t2-e2e`, `src-`/`build-f62f10833b44`, `/var/tmp/t1-probe`; yerel `m2dev-docs-wt`, `m2dev-a12fix`, stash.
