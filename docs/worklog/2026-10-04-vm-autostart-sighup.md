# VM açılışında sunucu kalkmıyor (rc.d servisi + SIGHUP)

- **Tarih:** 2026-10-04
- **Tür:** ortam
- **Alan:** runtime
- **Durum:** Taşındı → `docs/build-and-run.md` ("Başlat / durdur / durum")
- **PR / commit:** — (VM'de `/usr/local/etc/rc.d/m2dev`; yedeği `C:\Users\mertw\.m2dev\vm-scripts\m2dev.rc`)

## Problem / hedef
VM yeniden başlayınca sunucunun kendiliğinden kalkması.

## Kök neden / kanıt
- VM'de hiçbir otomatik başlatma yoktu (rc.d, crontab, rc.local boş); süreçler açılıştan sonra elle başlatılmıştı.
- `start.py`'yi rc.d'den çağıran ilk servis sürümünde süreçler açıldı ama 6 saniye sonra **6'sı aynı milisaniyede**
  `hupsig ... SIGHUP, SIGINT, SIGTERM signal has been received. shutting down.` yazdı (22:33:32.041) — `/etc/rc`'nin bittiği an.
- `start.py` çocuk süreçleri çağıranın terminaline bağlı bırakıyor; açılışta bu konsol. Konsol oturumu bitince SIGHUP gidiyor,
  game bunu kapanma sayıyor. SSH'tan (pty'siz) elle başlatınca sorun olmamasının nedeni de bu.

## Reddedilen yaklaşımlar
- Upstream `start.py`'yi değiştirmek: gerek yok, sorun çağırma biçiminde.
- `nohup`: game kendi SIGHUP handler'ını kurduğu için işe yaramaz; oturumdan tamamen ayırmak gerekir.

## Çözüm
`/usr/local/etc/rc.d/m2dev`: `REQUIRE: LOGIN mysql`, `start.py`'yi `daemon(8)` ile yeni bir oturumda çalıştırıyor;
stop'ta `stop.py` + süreçler kapanana kadar 30 sn bekleme. `/etc/rc.conf`: `m2dev_enable="YES"`, `m2dev_channels="1"`.

## Doğrulama
- `service m2dev start`: süreçler SID ayrı, TTY `-`.
- VM yeniden başlatma: 2 dk sonra 6 süreç ve 11 port ayakta.
- `service m2dev stop`: kanallar → auth → db sırası, db en son; 2 sn.

## Bir dahaki sefere tuzaklar
- `syserr.log` her açılışta sıfırlanır; kapanma kanıtını yeniden başlatmadan önce oku.
- FreeBSD `ps -o pid=,command=` çalışmıyor; `-o pid= -o command=` kullan.
