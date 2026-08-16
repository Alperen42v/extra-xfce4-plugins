[English README](README.md)
# Xfce4 Caffeine Panel Eklentisi

Ekranın kilitlenmesini/uykuya geçmesini engelleyen minimal bir Xfce panel eklentisi.

- **Sol tıklama**: caffeine modunu açar/kapatır.
- **Kapalı**: düz beyaz/çizgili kahve fincanı.
- **Açık**: sarı/dolgu renkli fincan ve basit, animasyonlu 2D buhar efekti.
- Standart `org.freedesktop.ScreenSaver` DBus `Inhibit` /
  `UnInhibit` arayüzünü kullanır. Bu sayede yalnızca Xfce'nin kendi güç
  yöneticisinde değil, farklı masaüstü ortamları ve ekran koruyucularla da çalışır.

## Derleme ve kurulum

Kendi Xfce panel eklentisi dizininize derleyip kurun (`sudo` gerekmez —
her şey kendi `$HOME` dizininizin altına kurulur. Bu işlem yalnızca
kullanıcınıza ait yapılandırmaya dokunur ve sistem dizinlerine asla müdahale etmez):

```bash
cd panel-plugin
make
make install
```

Bu işlem şunları kurar:

- `libcaffeine.so` → `$(pkg-config --variable=libdir libxfce4panel-2.0)/xfce4/panel-plugins/`
- `caffeine-plugin.desktop` → `$(pkg-config --variable=datadir libxfce4panel-2.0)/xfce4/panel-plugins/`
- Depodaki `icons/` klasöründe bulunan varsayılan simgeler → `~/.config/xfce4-caffeine-plugin/icons/`

Zaten burada özelleştirdiğiniz herhangi bir simge dosyasının üzerine yazılmaz.

Ardından yeni eklentinin yüklenmesi için paneli yeniden başlatın:

```bash
xfce4-panel -r
```

Şimdi panele sağ tıklayın → **Panel** → **Yeni Öğeler Ekle…** →
**"Caffeine"** araması yapın → **Ekle**.

## Özel Simgeler

Caffeine'in panel simgesini kendi tasarımınızla değiştirebilirsiniz.
Aşağıda belirtilen klasöre doğru adlara sahip PNG dosyalarını bırakmanız
yeterlidir. Tercihlerden bir tema seçmenin dışında herhangi bir ek ayar
gerekmez (aşağıdaki [Tercihler](#tercihler) bölümüne bakın).

Eklenti, paneli bir sonraki çizdiğinde bu dosyaları otomatik olarak yükler
(paneli yeniden boyutlandırabilir veya yeniden başlatmak için
`xfce4-panel -r` çalıştırabilirsiniz).

Bir dosya eksikse, Caffeine o durum için yerleşik Cairo ile çizilmiş fincan
simgesine geri döner. Böylece eksik veya kısmi bir simge seti hiçbir şeyi
bozmaz.

Simgeler **açık** ve **koyu** olmak üzere iki farklı sürüme sahiptir. Böylece
açık ve koyu panel/temalarda doğru görünebilirler. Hangi sürümün yükleneceği
**Özel simge teması** ayarı tarafından belirlenir. Ayrıntılar için
aşağıdaki [Tercihler](#tercihler) bölümüne bakın.

**Klasör:** `~/.config/xfce4-caffeine-plugin/icons/`

**Dosyalar:**

| Dosya | Durum | Açıklama |
|---|---|---|
| `off-light.png` | Caffeine KAPALI, açık sürüm | tek statik görüntü |
| `off-dark.png` | Caffeine KAPALI, koyu sürüm | tek statik görüntü |
| `on-light-01.png` | Caffeine AÇIK, açık sürüm | ilk (veya tek) animasyon karesi |
| `on-light-02.png`, `on-light-03.png`, ... | Caffeine AÇIK, açık sürüm | sırayla oynatılan ve döngüye alınan ek animasyon kareleri |
| `on-dark-01.png` | Caffeine AÇIK, koyu sürüm | ilk (veya tek) animasyon karesi |
| `on-dark-02.png`, `on-dark-03.png`, ... | Caffeine AÇIK, koyu sürüm | sırayla oynatılan ve döngüye alınan ek animasyon kareleri |

Kare numaraları iki haneli olacak şekilde sıfırla doldurulmalıdır
(`on-light-01.png`, `on-light-1.png` değil) ve arada boşluk olmamalıdır.
İlk eksik numarada yükleme durur.

Animasyon istemiyorsanız yalnızca `on-light-01.png` dosyasının bulunması
yeterlidir. Aynı durum `on-dark-01.png` için de geçerlidir.

Her iki sürümü de sağlamak zorunda değilsiniz. Örneğin yalnızca
`off-dark.png` ve `on-dark-*.png` varsa, açık sürümün kullanılması gereken
durumlarda Caffeine otomatik olarak yerleşik fincan simgesine geri döner.

**Biçim:** PNG. Simgenin panel arka planıyla bütünleşmesini ve düz bir kare
gibi görünmemesini istiyorsanız şeffaflık (alpha kanalı) kullanabilirsiniz.

Animasyonlu GIF doğrudan desteklenmez. Bunun yerine eklenti,
`on-light-NN.png` / `on-dark-NN.png` kareleri arasında yaklaşık 16 FPS
hızında geçiş yaparak kendi animasyonunu oluşturur. Bu hız, yerleşik buhar
animasyonuyla aynıdır.

Bir GIF düzenleyicisinde animasyon hazırlıyorsanız her kareyi ayrı bir PNG
olarak dışa aktarıp bu klasöre koymanız yeterlidir.

**Boyut:** Eklenti her PNG'yi panelin simge alanının piksel boyutuna tam
olarak uyacak şekilde ölçeklendirir. Bu nedenle kaynak görüntünün boyutu
teknik olarak fark etmez. Ancak en keskin sonucu elde etmek için
**64×64 piksel** olarak dışa aktarmanız önerilir.

Panel simge alanları panel boyutu ve DPI ayarlarına bağlı olarak genellikle
22–48 piksel civarındadır. Bu nedenle 64 piksel, bunların tamamı için yeterli
pay bırakır. Daha büyük bir kaynak küçültüldüğünde netliğini korur; daha küçük
bir kaynak büyütüldüğünde bulanıklaşır.

Kare olmayan kaynak görüntüler sığdırılmak için yatay/dikey olarak ezilebilir,
bu nedenle kare görüntüler kullanmanız önerilir.

## Tercihler

Panelde eklentiye sağ tıklayın → **Özellikler** seçeneğine tıklayarak
Caffeine'in tercih penceresini açabilirsiniz.

**Kilit döngüsü aralığı:** Caffeine AÇIK durumdayken, kendi belirlediğiniz
tekrarlayan bir programa göre ekranı kilitleyebilir (`xflock4`) ve monitörü
kapatarak boş bırakabilir (DPMS kapalı, yaklaşık 7 saniye sonra).

Aralık seçenekleri:

- 15 dakika
- 30 dakika
- 60 dakika
- Özel dakika değeri

Ardından aynı döngü devam eder.

Bu özellik Caffeine'in ne kadar süre açık kaldığından bağımsızdır:
Caffeine kapanmaz ve ekran koruyucu/DPMS engellemesi bu süre boyunca etkin
olmaya devam eder.

Varsayılan olarak **Never** seçilidir. Bu, orijinal davranışı korur:
ekranın kendi kendine kilitlenmesini tetiklemeden sonsuza kadar açık kalması.

**Özel simge teması:** Yukarıdaki [Özel Simgeler](#özel-simgeler) bölümünde
anlatılan özel simgelerin hangi sürümünün yükleneceğini belirler:

- **Auto** (varsayılan) — sistem/GTK temasını otomatik olarak takip eder
  (teknik olarak `gtk-application-prefer-dark-theme` ayarını kullanır).
  Böylece temayı elle seçmeniz veya sistem temasıyla senkronize tutmanız
  gerekmez.
- **Light** — sistem temasından bağımsız olarak her zaman `-light` dosyalarını kullanır.
- **Dark** — sistem temasından bağımsız olarak her zaman `-dark` dosyalarını kullanır.

Bu seçeneklerden herhangi birinin üzerine geldiğinizde şu açıklama
görüntülenir:

*"Sisteminizle eşleşen temayı seçmeniz önerilir."*

**Auto** seçeneği bunu sizin için otomatik olarak yaptığı için Light/Dark
seçenekleri çoğunlukla bunu manuel olarak değiştirmek istediğinizde veya
simge setinizin yalnızca tek bir sürümünü hazırladığınızda kullanılır.

Ayarlar `xfconf` üzerinden her eklenti örneği için ayrı ayrı saklanır.
Böylece aynı eklentinin birden fazla kopyası (örneğin farklı panellerde)
birbirinden bağımsız ayarlara sahip olabilir.

## Kaldırma

```bash
cd panel-plugin
make uninstall
xfce4-panel -r
```

Bu işlem eklenti ikili dosyasını ve `.desktop` dosyasını kaldırır. Ancak
`~/.config/xfce4-caffeine-plugin/icons/` klasörünü özellikle yerinde bırakır;
çünkü burada sizin özelleştirdiğiniz simgeler bulunabilir.

Tamamen temiz bir kurulum istiyorsanız klasörü kendiniz silebilirsiniz:

```bash
rm -rf ~/.config/xfce4-caffeine-plugin
```

## Notlar / Sonraki Adımlar

- `Inhibit` başarısız olursa (örneğin çalışan bir ekran koruyucu DBus servisi
  yoksa), eklenti görsel olarak **AÇIK** durumuna geçmez ve çalışıyormuş gibi
  davranmak yerine bir hata araç ipucu gösterir.
- Tercihler (kilit döngüsü aralığı) `caffeine-prefs.c` /
  `caffeine-prefs.h` dosyalarında bulunur.
- Özel simge yükleme işlemi `caffeine-icons.c` /
  `caffeine-icons.h` dosyalarında bulunur.
- Bu iki bölüm `caffeine.c` dosyasından ayrı tutulmuştur. Böylece temel
  engelleme/yaşam döngüsü mantığı daha düzenli kalır.
- İleride düşünülen fikirler:
  - Sistem askıya alma/devam ettirme işlemlerinden sonra Caffeine'i otomatik olarak yeniden etkinleştirmek.
  - Sabit klasör yerine Özellikler penceresine simge seçmek için dosya seçici eklemek.
  - Özel kilitleme/ekran kapatma komutları gibi daha fazla tercih seçeneği eklemek.
  - Tercihler arayüzünü yerelleştirmek (şimdilik yalnızca İngilizce).