@AGENTS.md

<!-- M2_LITE_BRAIN_START -->

## Claude Code notları

- Proje kuralları `AGENTS.md`'de duruyor (yukarıda içeri alındı); böylece bütün AI araçları tek bir kaynağı paylaşıyor. Kuralları orada düzenle, burada değil.
- `PackMaker.exe` / `pack.py` Git Bash'ten çalıştırılınca hiçbir şey üretmiyor; PowerShell aracını kullan.
- PowerShell 5.1 native exe'lere argüman geçirirken iç çift tırnakları siliyor (ör. `VBoxManage keyboardputstring`); bu tür komutlar için Bash aracını kullan.
- VM'e `ssh bsd` ile eriş. `ssh vm` (127.0.0.1:10022) **başka bir VM**'dir (MARTYSAMA), bu projeyle ilgisi yok.
- VM'de `syserr.log` dosyaları her süreç açılışında sıfırlanır; kapanma kanıtı arıyorsan yeniden başlatmadan önce oku.

<!-- M2_LITE_BRAIN_END -->
