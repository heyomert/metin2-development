# "Oyunu başlat" sonrası login'e geri atma

- **Tarih:** 2026-10-04
- **Tür:** ortam
- **Alan:** runtime
- **Durum:** Aktif
- **PR / commit:** — (sadece VM ortamı)

## Problem / hedef
GM hesabıyla girişte karakter seçilip "başlat"a basılınca client login ekranına dönüyordu.

## Kök neden / kanıt
- `channels/channel1/core1/syserr.log`: `GetServerLocation ... cannot find server for mapindex 41 (name [SA]Admin)`.
- Karakterler Jinno'da, map 41'de (`player.player.map_index`).
- Map 41 kanal 1'in core3'ünde yükleniyor (`server/channels.py:5`), ama VM'de kanal 1'den sadece core1 çalışıyordu.
  core2/core3'ün `CONFIG` dosyaları vardı, hiç başlatılmamışlardı (log dosyası bile yoktu).

## Reddedilen yaklaşımlar
- Karakteri veritabanında başka bir haritaya taşımak: belirtiyi gizler, Chunjo/Jinno karakterlerinin hepsi aynı sorunu yaşamaya devam ederdi.

## Çözüm
core2 ve core3 başlatıldı. Kalıcı çözüm: sunucu `service m2dev` ile upstream `start.py 1` üzerinden başlatılıyor,
bu da kanalın 3 çekirdeğini birden açıyor (`docs/worklog/2026-10-04-vm-autostart-sighup.md`).

## Doğrulama
core3 log'unda `AddGotoInfo(... mapIndex=41 ...)` ve P2P bağlantıları; kullanıcı oyuna girdi, `SAVE: [SA]Admin` görüldü.

## Bir dahaki sefere tuzaklar
- Belirti "login'e geri atıyor", ama client log'unda bir şey yok. Hata **kanal çekirdeğinin** `syserr.log`'unda.
- `start.py` dışında elle süreç başlatma; kanal başına bütün çekirdekleri açmayı unutmak kolay.
