# P2P portları kimlik doğrulamasız: pf ile dışarıya kapatma

- **Tarih:** 2026-10-05
- **Tür:** düzeltme / karar
- **Alan:** runtime (ağ)
- **Durum:** Aktif (test VM'de uygulandı; production'da uygulanması `docs/production-checklist.md` K-1 kapısı)
- **PR / commit:** (PR eklenecek)

## Problem / hedef
Çekirdekler arası (P2P) portlar dışarıya açık ve gelen bağlantıda kimlik doğrulaması yok; bağlanan herkes "başka bir çekirdek"
sayılıyor.

## Kök neden / kanıt
- P2P soketi public IP'ye bağlanıyor: `server-src/src/game/main.cpp:555` (iç IP'ye bağlayan satır yorum satırı, `:554`).
- Gelen P2P bağlantısı kontrolsüz kabul ediliyor: `game/desc_manager.cpp:108-134`.
- Alan çekirdek mesaja güveniyor: `GG::SHUTDOWN` → çekirdek 10 sn içinde kapanıyor (`game/input_p2p.cpp:470-474`).
- Bilgisayardaki 6 bağımsız Metin2 kaynağında aynı kod var; 4'ünde aynı yorum satırı. Açık sonradan eklenmemiş, orijinal tasarımın
  "bu portlara sadece sunucunun kendi süreçleri erişir" varsayımından geliyor. Hiçbirinde kodda koruma yok.
- Ölçüm (test VM, host'tan düz TCP bağlantısı): 12000/12011/12012/12013/12991 **erişilebilir**; `db` 9000 kapalı (127.0.0.1).
  Paket gönderilmedi, sömürü denenmedi; sadece bağlantı kurulabildiği ölçüldü.

## Reddedilen yaklaşımlar
- **P2P soketini iç IP'ye bağlamak (kod değişikliği):** her çekirdek `db`'ye *public IP*'sini bildiriyor, `db` bunu diğer çekirdeklere
  "P2P için buraya bağlan" diye dağıtıyor (`db/ClientManager.cpp:1162-1164`); aynı IP, oyuncuya warp adresi olarak da veriliyor
  (`db/ClientManager.cpp:1054`). Sadece P2P soketini iç IP'ye bağlarsak diğer çekirdekler hâlâ public IP'ye bağlanır ve reddedilir →
  P2P bozulur. Doğru yapmak game + db kodunu değiştirmeyi ve sunucuyu derlemeyi gerektirir (derleme komutu belgelenmemiş, A-9).
  Üstelik yüksek riskli; firewall aynı sonucu kod riski olmadan veriyor.
- **P2P'ye kimlik doğrulaması eklemek:** bütün çekirdekleri ve protokolü etkiler; çekirdekler tek makinedeyken gereksiz. Çekirdekler
  ayrı makinelere dağıtılırsa yeniden değerlendirilir.
- **Satır içi liste ile olumsuzlama (`from ! { a, b }`):** pf'te herkesle eşleşir; ilk denemede sözdizimi hatası verdi. Tablo kullanıldı.

## Çözüm
`deploy/freebsd/pf.conf`: P2P portlarına (`12000:12999`) dışarıdan gelen TCP'yi, `<m2dev_servers>` tablosundaki IP'ler dışında,
`em1` üzerinde sessizce düşürür. Hedefli engel; geri kalan her şey varsayılan "pass" (SSH, oyun portları, db etkilenmez).
Kod değişmedi. VM'de `/etc/pf.conf`, `pf_enable=YES`, `pf_rules=/etc/pf.conf`.

## Doğrulama (test VM)
| Test | Önce | Sonra |
|---|---|---|
| Dışarıdan TCP 12000, 12011–12013, 12991 | erişilebilir | **erişilemiyor** (yeniden başlatma sonrası da) |
| Dışarıdan TCP 11000, 11011, 22 | erişilebilir | erişilebilir |
| Kural sayacı | — | 25 paket engellendi (benim dış testlerim), iç trafikten engelleme yok |
| Sunucu yeniden başlatılınca çekirdekler arası bağlantılar | — | tam mesh kuruldu (4 çekirdek, 6 bağlantı), çekirdek `syserr.log`'larında hata yok |
| VM yeniden başlatma | — | `pf` ve sunucu açılışta kendiliğinden kalktı, mesh tam, hata yok |
| Oyun içi harita geçişi (GM `/goto`). **P2P'yi sınamaz:** warp adresi db'den gelir (`char.cpp:5371`); oyun portlarını, db'yi ve harita yüklemeyi sınar | — | **Geçti** (kullanıcı, 2026-10-05): map 1 (core1) → 21 (core2) → 41 (core3) → 113 (CH99) → 43 (core1), geçişlerde atılma yok; skill, yürüme, vuruş sorunsuz. Sunucu: 4 çekirdek ziyaret edildi, bütün `syserr.log`'lar boş, her geçişte `SAVE` yazıldı, P2P mesh (12 satır) sağlam |
| Oyun içi **P2P mesajı**: iki client, iki çekirdek, `/notice` ve `/transfer <isim>` | — | **Henüz yapılmadı.** P2P üzerinden gerçek mesaj geçtiğini gösterecek tek test bu. Mesh yeniden başlatma ve açılışta `pf` altında kuruldu ve açık kaldı, ama mesaj akışı ayrıca doğrulanmadı |

## Bir dahaki sefere tuzaklar
- **`pf` kural dosyasını CRLF ile kopyalama:** FreeBSD'ye gidecek dosyalar LF olmalı (`.gitattributes` → `deploy/** text eol=lf`).
- **Kiralık sunucuda arayüz adı ve IP düzeni farklı olacak** (`ext_if`, `<m2dev_servers>`). Kuralı uygularken zamanlayıcıyla geri alma kur
  (`docs/production-checklist.md`), yoksa SSH'ı kaybedebilirsin.
- Çekirdekler birbirine **kendi IP'leri üzerinden** bağlanıyor (soket çiftlerinin iki ucu da aynı IP). Kural bu yüzden tabloda
  sunucunun kendi IP'sini de içeriyor; makinenin kendi trafiği normalde `lo0` üzerinden döner (`set skip on lo0`) — bu davranış
  ayrıca ölçülmedi, tablo sayesinde kural ikisinde de doğru.
- Çekirdekleri ileride **birden çok makineye** dağıtırsan tabloya diğer makinelerin IP'lerini ekle.
- Bu sadece **P2P**'yi kapatır. Yönetim kanalı şifresi (K-3), şifre saklama (K-2), DDoS koruması ve tam host firewall (default-deny)
  ayrı kalemler (`docs/roadmap.md`).
