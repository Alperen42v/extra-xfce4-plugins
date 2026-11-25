[Türkçe](turkish-README.md)
# My KDE connect XFCE4 plugin
This plugin is a KDE connect plugin, designed to see your devices in the panel and to use or shortcut features in KDE Connect in the panel, as an XFCE4 plugin.
## Make and install
**Recommended**
```bash
./install.sh
# Then just follow the steps.
```
**Manual installation**
```bash
make install
xfce4-panel -r
# Then you can add it to your panel 
```

## Dependencies (Necessary then don't ask why it didn't work.)
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

## Notes