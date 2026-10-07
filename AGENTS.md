# AGENTS.md

<!-- M2_LITE_BRAIN_START -->

## Proje

d1str4ught'un M2Dev files'ı üzerine kurulu Metin2 files geliştirme projesi. Dört upstream repo tek bir
repoda birleştirilmiş, sürümleri [COMPONENTS.lock](COMPONENTS.lock)'ta sabitlenmiş. Aşama: geliştirme
(henüz production yok).

| Parça | Yol | Rolü |
|---|---|---|
| Client | `client/` | Runtime client: `assets/` (Python UI `assets/root`, locale, model/texture), `pack/` (üretilir), `Metin2.exe` |
| Client kaynağı | `client-src/` | C++ client; `Metin2.exe` ve `PackMaker.exe`'yi derler (CMake, VS 2022, x64) |
| Server | `server/` | Runtime verisi: quest'ler, SQL şemaları, proto `.txt`'leri, config, `start.py`/`stop.py` |
| Server kaynağı | `server-src/` | C++ `game` ve `db` (CMake, FreeBSD'de derlenir) |

Test ortamı: VirtualBox VM `M2DEV_FREEBSD151_ACCEPTANCE`, `192.168.56.20`, `ssh bsd` (anahtar tabanlı).
Sunucu VM'de `service m2dev` olarak çalışır. Ayrıntı: `docs/build-and-run.md`.

## Her oturumun başında

1. Bu dosyayı, sonra `docs/status.md`'yi (şu an / sıradaki / bekleyen kararlar) ve `docs/worklog/README.md`'yi (sadece indeks) oku. `status.md` eski olabilir: başındaki `git log` komutuyla kontrol et.
2. Sadece işle ilgili dokümanları aç:
   - `docs/architecture.md` — süreçler, kanallar/çekirdekler, veri akışı, yol sınıflandırması
   - `docs/build-and-run.md` — derleme, paketleme, quest derleme, VM, servis, MariaDB
   - `docs/game-facts.md` — level sınırı, imparatorluklar, haritalar, aktif sistemler/zindanlar
   - `docs/engineering/change-impact.md` — her anlamlı değişiklikten önce etki analizi
   - `docs/engineering/regression-baseline.md` — gerçek client regresyon senaryoları; hangileri gerekir: `change-impact.md` §7
   - `docs/roadmap.md` — fazlar, öncelikler, açık konular (kritik güvenlik bulguları dahil), teknik borç
   - `docs/production-checklist.md` — sunucu açılmadan önce geçilmesi gereken kapılar (P2P firewall dahil)
   - `docs/monitoring.md` — sunucu sağlık kaydı (lag, yük, oyuncu sayısı): alanlar ve okuma; "lag var mıydı" sorusuna önce buradan bak; production sorununda önce "Olay teşhisi" sırasını izle
   - `docs/backup.md` — DB yedeği, geri yükleme testi, felakette geri yükleme; veritabanına dokunmadan önce `m2dev-backup consistent`
3. Bir şey değiştirmeden önce gerçek dosyalarda doğrula. Dokümanlar yönlendirme ve hafızadır, kanıt değildir.

## Kurallar

- **Bu bir MMORPG: oyunun tamamını düşün.** Anlamlı her değişiklikten önce `docs/engineering/change-impact.md`'deki etki analizini yap ve PR'a ekle. Yüksek riskte (paket, DB şeması, item/yang akışı, çekirdekler arası, db önbelleği, auth) sıra: **analiz → onay → kod**. Derinlik risk seviyesine göre; her alan kanıtlı cevap, `Etkilenmiyor — neden` ya da `Bilinmiyor — ne kontrol edildi` olur, tahminle doldurulmaz; yüksek riskte kritik alan `Bilinmiyor` ise koda geçme.
- **Riskli istekte dur.** Yapılabilir ama stabiliteye/performansa/güvenliğe/veri bütünlüğüne zarar verebilecek bir istekte kodlamadan önce riskleri, alternatifleri ve önerini sun.
- **Client'a güvenme.** Hız, mesafe, miktar, sahiplik gibi kontroller sunucuda. Upstream client hile korumasını kaldırdı.
- **Çekirdek tek thread'li, db oyuncu verisini önbellekte tutar** (`docs/architecture.md` → "Çalışma modeli"): döngüye yavaş iş koyma; oyuncu verisini veritabanından doğrudan değiştirme.
- **Varsayım yok.** Her iddiayı `yol:satır`, komut çıktısı, log ya da testle destekle. Gerisini `Unverified` işaretle.
- **Dosyanın var olması ≠ aktif.** Server `game` build'i klasördeki bütün `.cpp`'leri derler (`GLOB_RECURSE`), bu yüzden aktifliği flag'ler, kayıtlar ve çağrı yolları belirler. Quest'ler ancak `server/share/locale/english/quest/locale_list`'te varsa derlenir.
- **Client exe her zaman `client-src`'den derlenir.** Upstream'in hazır exe'si kaynaktan eski kaldı ve login'i bozdu (`docs/worklog/2026-10-04-login-input-secret-mode.md`).
- **Kök nedeni düzelt.** Hata gizleyen yama yok (`hasattr` guard'ları, yutulan hatalar). Kod ile şema/config çelişirse dışarıda kalan tarafı bul ve onu düzelt.
- **Değişiklikten sonra kendi çözümünü çürütmeye çalış** (`docs/engineering/change-impact.md` §6, derinlik risk seviyesine göre). Yan etki, yeni hata yolu ve yanlış varsayım ara; kanıt çelişirse savunma, daralt ya da değiştir. Başarısız testi ürüne yüklemeden önce testi doğrula. Yüksek riskte, etkilenmemesi gereken başarı yolunu önce/sonra aynı senaryoyla kanıtla. PASS'in neyi kanıtlamadığını yaz.
- **Katmanlar arası değişiklikler birlikte yapılır:** client C++ ↔ Python UI ↔ server game ↔ db ↔ paketler ↔ proto/data ↔ quest'ler ↔ runtime config.
- **Diğer Metin2 kaynakları** (Masaüstündeki Anka2, MartySama, lorenzo vb.) bir bulguyu destekleyebilir; değişikliğin asıl dayanağı olamaz.
- **Asla commit'leme:** gizli bilgiler, `client/assets/root/serverinfo.py`'deki yerel VM IP'si, `client/config/locale.cfg`, `client/log/`, `client/pack/`, `client-src/build/`.
- **Gizli bilgiyi asla yazma** (doküman/commit/PR). VM bilgileri repo dışında `C:\Users\mertw\.m2dev\secrets\` içinde.
- **Önce sor:** commit/push/merge, VM'de servis yeniden başlatma, veritabanını değiştirme, bir şey silme.
- **Dil:** dokümanlar Türkçe; kod yorumları ve commit mesajları İngilizce; yollar/komutlar/teknik terimler olduğu gibi.

## Değişiklik akışı

- C++, Python↔C++ sınırı, paketler, proto'lar, DB şeması, quest/denge, güvenlik, release binary'leri, upstream güncellemeleri → branch + PR.
- Doküman, yazım hatası, `.gitignore`, geliştirme script'leri → doğrudan commit olur.
- PR metni: problem · kök neden/kanıt · değişiklik · test · gereken yeniden derleme/paketleme/migration.
- Aynı PR'da: etkilenen dokümanı güncelle ve açık olmayan bir şey öğrenildiyse worklog kaydı ekle.

## Durum dosyası

- `docs/status.md` her anlamlı iş ya da oturum bitince, **aynı commit'te** güncellenir: üzerine yaz, ekleme yapma, ~25 satırı geçme. Başındaki "Kapsadığı commit"i yeni duruma göre değiştir.
- Biten maddeyi sil (geçmişi git/worklog tutar); bekleyen kararları ve sıradaki işi kullanıcının göreceği şekilde yaz.

## Worklog

- `docs/worklog/YYYY-MM-DD-konu.md`, `_TEMPLATE.md`'den; `docs/worklog/README.md`'de kayıt başına bir satır. Aynı isim çakışırsa `-2` eki.
- Yaz: beklenmedik kök nedenler, reddedilen yaklaşımlar, ortam/build tuzakları, kararlar, önemsiz olmayan özellikler. Önemsiz düzenlemeler için yazma.
- PR'ı tekrarlama, linkini ver. Kalıcı dersleri bu dosyaya ya da dokümanlara taşı (`Taşındı`); eskiyenleri `Geçersiz` işaretle. Kayıt silinmez.

## Bu projede bilinen tuzaklar

- Upstream'in hazır `Metin2.exe`'si `ui.py`'den eski → `docs/worklog/2026-10-04-login-input-secret-mode.md`
- `pack.py --all` paralel çalışınca sessizce paket üretmiyor; ayrıca Git Bash'ten çalışmıyor → `docs/build-and-run.md`
- Oyuna girerken login'e geri atma = karakterin haritasını yükleyen çekirdek çalışmıyor → `docs/worklog/2026-10-04-channel-cores-map41.md`
- VM'de MariaDB `sql_mode` ve config dosyası izni → `docs/worklog/2026-10-04-mariadb-sql-mode.md`
- `start.py` açılışta SIGHUP ile ölüyor → `docs/worklog/2026-10-04-vm-autostart-sighup.md`
- Server listesindeki "Test" kaydı `127.0.0.1`'e gidiyor, kullanılmıyor; geliştirmede "01. Metin2" seçilir.
- Host belleği dolunca VirtualBox VM'i askıya alır; Windows bu hâldeyken yeniden başlarsa VM kesilir, bekleyen disk yazmaları kaybolur ve MariaDB Aria kaydı bozulabilir. Önce VM'i kapat (`ssh bsd shutdown -p now`) → `docs/worklog/2026-10-05-server-metrics.md`
- Çekirdekler arası P2P portları kodda kimlik doğrulamasız; sadece firewall (`deploy/freebsd/pf.conf`) korur. Yeni sunucuda bu kural şart → `docs/production-checklist.md`, `docs/worklog/2026-10-05-p2p-firewall.md`

<!-- M2_LITE_BRAIN_END -->
