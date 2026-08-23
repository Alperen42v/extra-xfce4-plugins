[English README](./README.md)
# Night Light — XFCE4 Panel Eklentisi

XRandR gamma kullanarak ekranın renk sıcaklığını (mavi ışık) ayarlayan,
zamanlama ve yumuşak geçiş desteğine sahip bir XFCE4 panel eklentisi.

## Özellikler

- **Sıcak ışık seviyesi**: 0-100 arasında bir kaydırıcı ile renk sıcaklığına
  (K) dönüştürülür ve XRandR'nin `XRRSetCrtcGamma` işlevi kullanılarak uygulanır.
  Bu, redshift/gammastep tarafından kullanılan teknikle aynıdır.
- **Zamanlayıcı**: Başlangıç ve bitiş saati belirlenebilir. 24 saatlik veya
  12 saatlik (AM/PM) format arasında seçim yapılabilir. Gece yarısını aşan
  zaman aralıkları desteklenir (örneğin 22:00 -> 07:00).
- **Yumuşak geçişler**: Açma/kapatma sırasında gamma değeri kademeli olarak
  değiştirilir. Geçiş süresi ayarlanabilir.
- Panel simgesine tıklayarak manuel olarak açıp kapatabilirsiniz
  (zamanlamayı geçici olarak geçersiz kılar).
- Ayarlar `~/.config/xfce4/panel/nightlight-<id>.rc` altında saklanır.

## Derleme ve kurulum

```bash
cd nightlight-plugin
make
make install
xfce4-panel -r
```

`make install` dosyaları yalnızca
`~/.local/lib/xfce4/panel/plugins/` ve
`~/.local/share/xfce4/panel/plugins/` dizinlerine kopyalar
(yalnızca kendi kullanıcı hesabınıza).

`xfce4-panel -r`, eklentinin listede görünmesi için paneli yeniden başlatır.

## Panele ekleme

Panele sağ tıklayın → **Panel** → **Yeni Öğeler Ekle...** →
**"Night Light"** araması yapın → **Ekle**.

Eklendikten sonra panel simgesine sağ tıklayın ve ayarlar penceresini açmak
için **Özellikler** seçeneğini seçin. Buradan sıcaklık seviyesi, zamanlama,
format ve geçiş süresi gibi ayarları değiştirebilirsiniz.

## Kaldırma

```bash
make uninstall
```

## Notlar / Bilinen sınırlamalar

- Çoklu monitör kurulumlarında aynı renk sıcaklığı her CRTC'ye uygulanır.
- Aynı anda başka bir gamma aracı (örneğin `redshift` veya `gammastep`)
  çalıştırmayın; ayarlar birbiriyle çakışacaktır.
- XFCE şu anda X11 (Xorg) tabanlıdır; bu eklenti Xorg gerektirir
  (Wayland desteği yoktur).
- Çıkış sırasında (`free-data`) ekran otomatik olarak nötr renk sıcaklığına
  (6500K) geri döndürülür.