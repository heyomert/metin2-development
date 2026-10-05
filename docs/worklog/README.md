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
| 2026-10-05 | [Yazı çizimi: FreeType LCD yerine orijinal GDI + 1-bit eşik (Anka2 görünümü)](2026-10-05-font-gdi-render.md) | düzeltme / karar | client-src | Aktif | — |

<!-- Eski yıllar: README-<yıl>.md -->
