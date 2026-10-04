# Yönetim kanalı şifresi ve conf izinleri (K-3, config adımı)

- **Tarih:** 2026-10-05
- **Tür:** düzeltme / ortam
- **Alan:** runtime (config)
- **Durum:** Aktif — config adımı test VM'de yapıldı; kod adımı ve production bekliyor (`docs/roadmap.md` 1.3)
- **PR / commit:** (PR eklenecek)

## Problem / hedef
Yönetim kanalının şifresi upstream'deki varsayılan değerdi ve sunucu config dosyaları herkese yazılabilirdi.
Bulguların ayrıntısı: `docs/roadmap.md` K-3, A-11, A-12.

## Çözüm (sadece config, kod değişmedi)
- VM `game.txt`: `ADMINPAGE_PASSWORD` rastgele 32 karakterlik bir değerle değiştirildi; değer ekrana basılmadan üretildi,
  kopyaları repo dışında (VM `/root/.m2dev-admin-channel-password`, host `C:\Users\mertw\.m2dev\secrets\admin-channel-password`).
- `ADMINPAGE_IP: 127.0.0.1` korundu (boş bırakılmamalı).
- VM `conf/` dosyaları `640`, klasör `750` (önce `666`/`777`; sebebi upstream `server/perms.py`).
- Repodaki `server/share/conf/game.txt`: şifre satırı `CHANGE_ME_BEFORE_PRODUCTION` yer tutucusu oldu.
- Yedek: VM `/root/k3-backup/` (eski `game.txt` ve izin listesi).

## Reddedilen yaklaşımlar
- Sadece config ile yetinmek: şifrenin log'a düşme yolu kodda (`docs/roadmap.md` K-3) — kod adımı ayrıca yapılacak.

## Doğrulama (test VM)
- Eski ve yeni şifre satırının özeti farklı; dosyada şifre satırı dışında fark yok (sadece dosya sonuna satır sonu eklendi).
- `service m2dev` yeniden başlatıldı: 6 süreç ayakta, P2P mesh tam (12 satır), bütün `syserr.log`'lar boş.
- Yeni şifre hiçbir log dosyasında geçmiyor (sayımla kontrol edildi, değer basılmadı).
- Yönetim kanalına komut gönderilerek test yapılmadı.

## Bir dahaki sefere tuzaklar
- `server/perms.py` yeniden çalıştırılırsa izinler `777`'ye döner.
- Repodaki `game.txt` artık yer tutucu içeriyor; repodan kopyalanan config'te şifre yeniden ayarlanmalı.
- Config değişikliği `service m2dev` yeniden başlatmadan etkin olmaz.
