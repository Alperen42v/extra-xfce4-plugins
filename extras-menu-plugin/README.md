[Türkçe](turkish-README.md)
# Extras Menu

XFCE4 panel plugin. It provides a pop-up window opened with a single button
on the panel, similar to GNOME's "Quick Settings" menu: volume and screen
brightness sliders, as well as quick toggle buttons such as Bluetooth /
Dark Mode / Aeroplane Mode.

It is currently in the alpha stage. The volume and brightness sliders
are connected to the actual system controls; the remaining buttons are
currently just visual placeholders.

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

After installation, restart the panel:

```bash
xfce4-panel -r
```

It can be added to the panel by right-clicking → Panel → Add New Items...
and selecting "Extras Menu".

### Notes

1. **Bluetooth toggle could not be tested:** Since the developer's hardware does not have a Bluetooth adapter, the backend for enabling/disabling the adapter could not be tested end-to-end on real hardware.The code logic appears to be solid, but it needs to be verified by someone with an actual Bluetooth adapter. If you encounter any issues, you can open an issue.
