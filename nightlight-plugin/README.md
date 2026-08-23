[Türkçe README](./turkish-README.md)
# Night Light — XFCE4 Panel Plugin

An XFCE4 panel plugin that adjusts the screen's color temperature (blue
light) via XRandR gamma, with a schedule and smooth transitions.

## Features

- **Warm light strength**: a 0-100 slider, mapped to a color temperature (K)
  and applied via XRandR's `XRRSetCrtcGamma` (the same technique used by
  redshift/gammastep).
- **Scheduler**: start and end time, with a choice of 24-hour or 12-hour
  (AM/PM) format. Ranges that wrap past midnight are supported (e.g. 22:00 -> 07:00).
- **Smooth transitions**: turning on/off gradually eases the gamma value
  in/out; the transition duration is adjustable.
- Click the panel icon to toggle manually (temporarily overrides the schedule).
- Settings are stored under `~/.config/xfce4/panel/nightlight-<id>.rc`.

## Build and install

```bash
cd nightlight-plugin
make
make install
xfce4-panel -r
```

`make install` copies files only into
`~/.local/lib/xfce4/panel/plugins/` and
`~/.local/share/xfce4/panel/plugins/` (your user account only).
`xfce4-panel -r` restarts the panel so the plugin shows up in the list.

## Adding it to the panel

Right-click the panel → **Panel** → **Add New Items...** → search for
**"Night Light"** → **Add**.

Once added, right-click the panel icon and choose **Properties** to open
the settings dialog (strength, schedule, format, transition duration).

## Uninstall

```bash
make uninstall
```

## Notes / known limitations

- On multi-monitor setups the same temperature is applied to every CRTC.
- Don't run another gamma tool (e.g. `redshift`/`gammastep`) at the same
  time — the settings will conflict.
- XFCE is currently X11 (Xorg) based; this plugin requires Xorg (no
  Wayland support).
- On exit (`free-data`), the screen is automatically restored to neutral
  (6500K).
