# Worklog

Açık olmayan bir şeyin öğrenildiği düzeltme ve özelliklerin indeksi. Kayıt başına bir satır.
Ayrıntı dosyasını sadece mevcut işle ilgiliyse aç.

Kurallar: bkz. `AGENTS.md` → Worklog. Yeni kayıt: `_TEMPLATE.md`'yi `YYYY-MM-DD-kisa-konu.md`
olarak kopyala (aynı gün isim çakışırsa `-2`).

| Tarih | Kayıt | Tür | Alan | Durum | PR |
|---|---|---|---|---|---|
| 2026-10-04 | [Login kutularına yazı yazılamıyor (eski hazır exe)](2026-10-04-login-input-secret-mode.md) | düzeltme | client | Taşındı → AGENTS.md | #1 |
| 2026-10-04 | ["Oyunu başlat" sonrası login'e geri atma](2026-10-04-channel-cores-map41.md) | ortam | runtime | Aktif | — |
| 2026-10-04 | [VM açılışında sunucu kalkmıyor (SIGHUP)](2026-10-04-vm-autostart-sighup.md) | ortam | runtime | Taşındı → build-and-run.md | — |
| 2026-10-04 | [db proto kopyalama hataları (sql_mode + izin)](2026-10-04-mariadb-sql-mode.md) | ortam | db | Taşındı → build-and-run.md | — |
| 2026-10-04 | [loginlog2.is_gm 'Y'/'N' vs int](2026-10-04-loginlog2-is-gm.md) | düzeltme / karar | db | Aktif | #2 |
| 2026-10-04 | [loginlog2.playtime TIME vs int](2026-10-04-loginlog2-playtime.md) | düzeltme / karar | db | Aktif | #3 |
| 2026-10-05 | [P2P portları kimlik doğrulamasız: pf ile dışarıya kapatma](2026-10-05-p2p-firewall.md) | düzeltme / karar | runtime | Aktif | #5 |
| 2026-10-05 | [Yönetim kanalı şifresi ve conf izinleri (K-3, config)](2026-10-05-admin-channel-config.md) | düzeltme / ortam | runtime | Aktif | #6 |
| 2026-10-05 | [Server derleme doğrulandı; MariaDB kütüphanesi farklı ayarla derlenmiş](2026-10-05-server-build-verify.md) | ortam | build | Taşındı → build-and-run.md | — |
| 2026-10-05 | [Yönetim kanalı: port güvenliği geri getirildi, şifre log'larda maskelendi (K-3, kod)](2026-10-05-admin-channel-code.md) | düzeltme / karar | server | Aktif | #7 |
| 2026-10-05 | [Hesap şifreleri (K-2): durum doğrulandı, A ve B uygulandı](2026-10-05-k2-password-review.md) | karar / düzeltme | server / db | Aktif | — |
| 2026-10-05 | [Yazı çizimi: FreeType LCD yerine orijinal GDI + 1-bit eşik (Anka2 görünümü)](2026-10-05-font-gdi-render.md) | düzeltme / karar | client-src | Aktif | #9 |
| 2026-10-05 | [config.exe kaynaktan yeniden yazıldı; tam ekranda frekans seçimi MSAA'yı kapatıyordu](2026-10-05-config-tool.md) | düzeltme / karar | client-src / client | Aktif | #10 |
| 2026-10-05 | [Zemin dokuları bulanıktı: DX9 geçişinde anizotropi ayarı kaybolmuş](2026-10-05-anisotropic-filtering.md) | düzeltme | client-src | Aktif | #11 |
| 2026-10-05 | [Sunucu sağlık kaydı (metrics); VM askıya alınınca Aria kaydı bozuldu](2026-10-05-server-metrics.md) | özellik / karar / ortam | server-src / runtime | Aktif | #12 |
| 2026-10-05 | [DB yedeği: mantıksal döküm, şifreli, dışarıdan çekme, geri yükleme testi; `mariadb-backup` Aria'da geri yüklenemedi](2026-10-05-db-backup.md) | özellik / karar / ortam | runtime / db | Aktif | #13 |
| 2026-10-06 | [AsyncSQL davranış baseline'ı: hata enjeksiyonu testleri (DB adım 1a)](2026-10-06-asyncsql-baseline.md) | karar / ortam | server-src / tools | Aktif | #14 |
| 2026-10-06 | [MariaDB + OS toplayıcısı `m2dev-dbstat` (DB adım 1b); süreç CPU'su ms'ye kesilince kayboluyordu](2026-10-06-dbstat-collector.md) | özellik / karar / ortam | runtime / tools | Aktif | #15 |
| 2026-10-06 | [AsyncSQL kuyruk/bekleme/hata sayaçları (DB adım 1c); cache flush SAVE'leri peer'siz erken dönüşe düşüyor; `uiSQLErrno` tekrar sonrası temizlenmiyor](2026-10-06-sql-counters.md) | özellik / karar / ortam | server-src / runtime / tools | Aktif | #17 |
| 2026-10-06 | [Derleme kimliği ve game+db kurulumu (T-2); yanlış pathspec, yok sayılan-derlenen kaynak, FreeBSD make saniye çözünürlüğü](2026-10-06-build-identity.md) | özellik / karar / ortam | server-src / build / runtime / tools | Aktif | #20 |
| 2026-10-07 | [`syserr.log` yeniden başlatmada korunuyor (T-1); libc++ `symlink_status` tuzağı, çökmede satır kaybı](2026-10-07-syserr-archive.md) | özellik / karar | server-src / runtime / tools | Aktif | #21 |
| 2026-10-07 | [AsyncSQL düzeltmesi (DB adım 2a): sonuç/politika ayrımı, kör tekrar kaldırıldı, üst veri logları; `mysql_real_query` `-1` okuma evresinde de dönüyor, çoklu ifade hatası görünmüyor](2026-10-07-asyncsql-2a.md) | düzeltme / karar / ortam | server-src / runtime / tools | Aktif | #22 |
| 2026-10-07 | [PR #22 test VM kabul testi ve regresyon tabanı; "şimdi" sinyali ve önbellek/kalıcılık tuzakları](2026-10-07-pr22-acceptance.md) | karar / ortam | server-src / runtime / tools | Aktif | #22 |
| 2026-10-07 | [Depo ödeme kapısı sunucuda yok (A-23); mall depo kapısını açıyordu; karakter silme ve aktivasyon probe'ları](2026-10-07-safebox-a23.md) | düzeltme / karar | server-src / server / db | Aktif | — |
| 2026-10-08 | [Depo hesap aktivasyonu (A-19): idempotent satır, blokajlı ücret, UNKNOWN/INACTIVE/ACTIVE; `money_log` quest ücretini yazmıyor; test VM kabulü PASS, UNKNOWN için tablo kilidi](2026-10-08-safebox-a19.md) | özellik / düzeltme / karar | server-src / server / tools | Aktif | — |
| 2026-10-10 | [İsim değiştirme DB hatasında item'ı siliyordu (A-17 g19, g18 kontrolü): senkron UPDATE düşerken asenkron yan etkiler DB dönünce uygulanıyordu; "Karakter değiştir" listesi ve oyundaki ad kanıt değil; test VM önce/sonra PASS](2026-10-10-change-name-a17.md) | düzeltme / karar | server-src / server / tools | Aktif | #27 |
| 2026-10-11 | [Lonca kurma INSERT'i başarısız olunca hayalet lonca 0 ve `UNIQUE pid` ile kalıcı üyelik kilidi; sonraki kurulumda ücret alınıp lider eklenmiyordu (A-17 g11); yedek işaret dosyalarını içermiyor](2026-10-11-guild-create-a17.md) | düzeltme / karar | server-src / db / tools | Aktif | #28 |
| 2026-10-11 | [Lonca savaşı bahsi: d29 "telafi yok" yanlıştı (iade `item_award` ile var); İngilizce bahis quest'i `_80_say` biçim hatasıyla her denemede çöküyordu; yalnız `translate.lua` yükleniyor, geri düşme yok](2026-10-11-guild-war-bet-quest.md) | düzeltme / karar | server / db | Aktif | #29 |

<!-- Eski yıllar: README-<yıl>.md -->
