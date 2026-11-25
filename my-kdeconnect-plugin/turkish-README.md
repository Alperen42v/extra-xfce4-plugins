[English](README.md)
# My KDE connect XFCE4 plugini
Bu plugin KDE connect bağlı bir XFCE4 panel eklentisidir. amacı panelde cihazlarınızı görmek ve KDE connect'deki özellikleri panelde kısayol olarak görmek veya kullanmak için yapılmış XFCE4 eklentisidir.
## Derleme Kurulum
**Önerilen**
```bash
./install.sh
# sonra adımları takip edin yeter.
```
**Manuel kurulum**
```bash
make install
xfce4-panel -r
# sonra panelinize ekleye bilirsiniz 
```
## Bağımlılıklar (gereklidir sonra neden çalışmadı demeyin.)
### Ubuntu / Debian
```bash
sudo apt install base-devel pkgconf gtk3 xfce4-panel libxfce4util kdeconnect libarchive
```
### Arch linux
```bash
sudo pacman -S --needed base-devel pkgconf gtk3 xfce4-panel libxfce4util kdeconnect libarchive
```
### Fedora
```bash
sudo dnf install  base-devel pkgconf gtk3 xfce4-panel libxfce4util kdeconnect libarchive
```
## Notlar