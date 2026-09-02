[English](README.md)
# Extras Menu

XFCE4 panel eklentisi. GNOME'un "hızlı ayarlar" menüsüne benzer, panelde
tek bir düğmeyle açılan bir açılır pencere sunar: ses ve ekran parlaklığı
kaydırıcıları, ayrıca Bluetooth / Dark Mode / Aeroplane Mode gibi hızlı
geçiş düğmeleri.

Şu an alpha aşamasında. Ses ve parlaklık kaydırıcıları
gerçek sistem denetimlerine bağlı; geri kalan düğmeler henüz görsel
yer tutucu.

## Bağımlılıklar

### Arch Linux

```bash
sudo pacman -S brightnessctl --needed
```

### Debian / Ubuntu

```bash
sudo apt install brightnessctl
```

### Fedora

```bash
sudo dnf install brightnessctl
```

## Derleme ve kurulum

```bash
make
make install
```

Kurulumdan sonra paneli yeniden başlat:

```bash
xfce4-panel -r
```

Panelde sağ tık → Panel → Öğe Ekle... üzerinden "Extras Menu" olarak eklenebilir.

## Notlar

1.**Bluetooth toggle test edilemedi:** Geliştirici donanımında Bluetooth
adaptörü bulunmadığı için adaptör açma/kapama backend'i gerçek donanımda uçtan uca test edilemedi.
Kod mantığı sağlam ancak gerçek bir
Bluetooth adaptörü olan biri tarafından doğrulanması gerekiyor. Sorun
yaşarsan issue açabilirsin.