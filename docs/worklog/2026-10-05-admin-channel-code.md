# Yönetim kanalı: port güvenliği geri getirildi, şifre log'larda maskelendi (K-3 kod adımı)

- **Tarih:** 2026-10-05
- **Tür:** düzeltme / karar
- **Alan:** server-src (game), runtime
- **Durum:** Aktif — test VM'de devrede; production `docs/production-checklist.md` K-3
- **PR / commit:** (PR eklenecek)

## Problem / hedef
Yönetim kanalının şifresinin log dosyalarına düşmesini kalıcı olarak engellemek.

## Kök neden / kanıt
- Şifreyi log'a yazabilen 3 satır: `game/input.cpp:238` (seviye 0, Release'de **yazılıyor**), `input.cpp:518` ve `config.cpp:198`
  (seviye 1 = `debug`, sadece Debug derlemede). Seviye eşlemesi `libthecore/log.cpp:89-100`, Release log seviyesi `log.cpp:54-58`.
- Upstream'in ilk sürümünde `ENABLE_PORT_SECURITY` açıktı (`common/service.h`): `ADMINPAGE_IP` dışındaki kaynaklardan gelen metin
  komutları işlenmeden ve içerik log'lanmadan reddediliyordu. Upstream `5c9ae80b`'de (2025-08-22, "removed pointless
  ENABLE_PORT_SECURITY define") kontrolü **sildi** (her zaman açık yapmadı; diff `input.cpp` −8 satır).
- 4 bağımsız kaynakta (MartySama 5.9, lorenzo, Andesia, 2008–2010 TR) aynı kontrol açık.
- Oyun client'ı bu kanalı kullanmıyor (`client-src/src/EterLib/NetStream.cpp:288` sadece debug adı tablosu); repoda kullanan araç yok.
- Log arşivleri 7 gün tutuluyor (`libthecore/syslog_rotate_sink.h:63`) ve `644`.

## Reddedilen yaklaşımlar
- **Sadece log maskeleme:** liste dışı kaynakların kanala erişimi açık kalırdı (roadmap A-11).
- **Korumayı `#ifdef ENABLE_PORT_SECURITY` ile geri getirmek:** yanlışlıkla kapatılabilir; koşulsuz eklendi.

## Çözüm (kod: `server-src/src/game/input.cpp`, `config.cpp`)
1. `HandleText` başında port güvenliği: liste boşsa ya da kaynak listede değilse içerik log'lanmadan bağlantı kapatılır (fail closed).
2. `SOCKET_CMD` log satırında komut şifreyse `<admin password>` yazılır.
3. Debug `TEXT … RESULT` satırında aynı maskeleme.
4. Açılışta şifre yazılmaz; şifre bilinen varsayılan/yer tutucuysa `syserr`'e uyarı.

## Doğrulama (test VM)
- Derleme: sadece `input.cpp` ve `config.cpp` yeniden derlendi, 0 hata, bu dosyalarda 0 uyarı. Sadece `game` devreye alındı.
- Açılış: 6 süreç, P2P mesh tam, bütün `syserr.log` boş (varsayılan-şifre uyarısı yok).
- Oyun testi (kullanıcı): giriş, karakter yükleme, eşya taşıyıp çık-gir, `/goto #1/#21/#41`, yürüme/vuruş/skill — sorun yok.
  Log: 7 başarılı giriş, her geçişte `SAVE`, yeniden girişte son konumdan yükleme, `loginlog2` kayıtları doğru.
- Şifre mevcut log'larda ve arşivlerde 0 kez geçiyor (değer basılmadan sayıldı).
- Yönetim kanalının kendisine komut gönderilerek test yapılmadı.

## Bir dahaki sefere tuzaklar
- Bu, upstream'den bilinçli bir ayrışma (A-8): upstream güncellemesi alınırken bu kontrolün korunduğunu kontrol et.
- `ADMINPAGE_IP` boşsa kanal herkese kapalı; Faz 3 yönetim servisi listedeki bir IP'den bağlanmalı.
- `db` ve `qc` hâlâ 4 Ekim derlemesinden; ileride `db` değiştiğinde DB bağlantısı yeniden test edilmeli.
