[Türkçe](turkish-README.md)
# Extras Menu

XFCE4 panel plugin. It provides a pop-up window opened with a single button
on the panel, similar to GNOME's "Quick Settings" menu: volume and screen
brightness sliders, as well as quick toggle buttons such as Bluetooth /
Wifi/ethernet / Aeroplane Mode.

It is currently in the alpha phase, and the working buttons are Wifi/ethernet, brightness control, volume control, small buttons above (power button, screenshot button, etc.) **still being added one by one**

## Pictures
![adwaita elementary-icons](./screenshots/screenshot0-adwaita-elementary-icon.png)
![big-sur-dark](./screenshots/screenshot1-bigsur-dark.png)
![arc-dark_kora-icons](./screenshots/screenshot2-arc-dark_kora-icons.png)
## Dependencies

### Arch Linux

```bash
sudo pacman -S brightnessctl --needed
````

### Debian / Ubuntu

```bash
sudo apt install brightnessctl
```

### Fedora

```bash
sudo dnf install brightnessctl
```

## Build and Installation

```bash
make
make install
```

After installation, restart the panel (Recommended):

```bash
xfce4-panel -r
```

It can be added to the panel by right-clicking → Panel → Add New Items...
and selecting "Extras Menu".

### Notes
1. It's called extras menu now, but I'm planning to do something like Gnome-quick-settings or Quick-settings-Gnome-style in the future. 
2. I will do install.sh in the future, but I am currently in code logic and I continue to develop. (I will do it soon.) 
3. I added some things for example, Dark Mode or Balanced random, I plan to make the default layout more useful when it develops more in the future.
