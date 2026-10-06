# Durum

Kapsadığı commit: T-1 dalı `feat/syserr-preserve` (2026-10-07; taban 89025db8). **Üzerine yaz, ekleme yapma**; en fazla ~25 satır.
Güncel mi? `git log --oneline 89025db8..HEAD -- . ':!docs/status.md'` T-1 dalı dışında bir şey gösteriyorsa eski olabilir.

## Şu an
- **T-2 / 1.9:** test VM tamam (PR #20); **production temiz-git release yolu açık** → production kurulumu fail-closed.
- **T-1** kodlandı (`log/syserr_*.log`, N=30, `flush_on(err)`); korumalı testler, 200 bin satır A/B, `sqlrt` geçti; çalışan süreçte ☐.
  Bulgular (roadmap): db "End of pid" yazmıyor, A-14 sınırsız `syserr`, `CHECKPOINT` satırı kayboluyor.
- **DB standardı** (`docs/engineering/db-standard.md`): 1a/1b/1c bitti, S12 düzeltme adımında; yedek + sağlık kaydı çalışıyor. Production ☐.

## Sıradaki (her adım ayrı etki analizi + onay)
1. **T-1** PR incelemesi/merge → gerçek commit'ten derleme → ayrı onayla test VM uçtan uca (yeniden başlatmada arşiv, `kill -9` sonrası)
2. **AsyncSQL reliability fix** (1a testleri tersine dönmeli) → 3. **Şema/migration standardı** → 4. **InnoDB dönüşümü**
Bağımsız: T-3, T-4, `db.core`, A-14 salt okuma analizi, A-12, production release yolu, tekrar üretilebilirlik, ticaret+trigger testi.

## Senden bekleyen kararlar
- `heart_idle` fazladan pulse (yük testinden önce analiz?); binlog + PITR gerekli mi; uzun ad taşması; freetype silinsin mi; DPI
- A-1 hile tepki politikası; özellik listesi (F-1); production sunucu sağlayıcısı (G-3, G-4)

## Ortam
Commit'lenmeyen yereller: `serverinfo.py` (VM IP), `locale.cfg`, `metin2.cfg`. GM `admin` (şifre repo dışında); `Metin2.exe` yönetici.
Test VM: `pf` aktif, `m2dev_dbstat` açık; game/db = `f62f10833` (`src=archive`, `share/bin/BUILD`); önceki 1c çifti
`share/bin/.prev.20261006T202254Z.51584/` (kimliksiz → elle geri dönüş, `docs/build-and-run.md`). Windows'tan önce VM'i kapat.
Silme onayı bekleyen: `/root` DB kopyaları (`mysql-cold-*`, `aria-log-corrupt-*`); geçici `/root/{t2a,t2b,build-t2a,build-t2b,t2-tools,
1c-tools,src-1c,t2-e2e,src-t1,build-t1}`, `/root/*f62f10833b44*`, `/var/tmp/t1-probe`; yerel `m2dev-docs-wt`, `m2dev-a12fix`, stash.
