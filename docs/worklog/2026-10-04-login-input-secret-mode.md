# Login kutularına yazı yazılamıyor (eski hazır Metin2.exe)

- **Tarih:** 2026-10-04
- **Tür:** düzeltme
- **Alan:** client / client-src
- **Durum:** Taşındı → `AGENTS.md` (kural: exe her zaman kaynaktan derlenir)
- **PR / commit:** https://github.com/heyomert/metin2-development/pull/1

## Problem / hedef
Login ekranında ID ve şifre kutularına tıklanınca imleç çıkmıyor, yazı yazılamıyordu.

## Kök neden / kanıt
- `client/log/syserr.txt`: `ui.py line 729, in OnSetFocus` → `AttributeError: module 'ime' has no attribute 'SetSecretMode'`.
  Hata `ime.EnableCaptureInput()` ve `wndMgr.ShowCursor()` satırlarından önce çıktığı için odak hiç tamamlanmıyordu.
- Upstream'deki hazır `Metin2.exe` 2026-02-16'da derlenmiş, içinde `SetSecretMode` adı geçmiyor.
- `ime.SetSecretMode` client-src'ye 2026-02-17'de eklendi (`c687bf7`); `ui.py` 2026-02-18'de (upstream PR #69) bunu çağırmaya başladı.
- Yani Python yeni API'yi kullanıyor, hazır exe hiç yeniden derlenmemiş.

## Reddedilen yaklaşımlar
- `ui.py`'de `if hasattr(ime, "SetSecretMode")` yaması (önceki bir agent `test-files` kopyasında bunu yapmıştı):
  hatayı gizliyor ve şifre alanındaki kopyalama/kesme korumasını sessizce kapatıyor
  (`client-src/src/UserInterface/PythonIME.cpp:104`, `:192`).

## Çözüm
`client-src` (`a7555110`) Release x64 derlendi, `client/Metin2.exe` ile değiştirildi. Python ya da C++ kodu değişmedi.

## Doğrulama
Kullanıcı login kutularına yazabildi ve giriş yaptı. Yeni `syserr.txt`'de `SetSecretMode` hatası yok.

## Bir dahaki sefere tuzaklar
- Upstream client güncellenince hazır exe yine eski kalabilir. Python'un çağırdığı `ime.*`/`net.*` fonksiyonlarının exe'de olduğunu kontrol et.
- `syserr.txt`'deki `invalid idx 0` (MarkManager, lonca amblemi) bu sorunla ilgili değil.
