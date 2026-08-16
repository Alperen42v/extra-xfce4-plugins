# Xfce4 Caffeine Panel Plugin

A minimal Xfce panel plugin that keeps your screen from locking/sleeping.

- **Left click**: toggles caffeine mode on/off.
- **Off**: plain white/outline coffee cup.
- **On**: yellow/filled cup with a simple animated 2D steam wisp.
- Uses the standard `org.freedesktop.ScreenSaver` DBus `Inhibit` /
  `UnInhibit` interface, so it works across desktops/screensavers, not
  just Xfce's own power manager.

## Build & install

Build and install into your user's Xfce panel plugin directory (no
`sudo` — everything installs under your own `$HOME`, this only touches
your user's own config, never system directories):

```bash
cd panel-plugin
make
make install
```

This installs:
- `libcaffeine.so` into `$(pkg-config --variable=libdir libxfce4panel-2.0)/xfce4/panel-plugins/`
- `caffeine-plugin.desktop` into `$(pkg-config --variable=datadir libxfce4panel-2.0)/xfce4/panel-plugins/`
- the bundled default icons (from `icons/` in this repo) into `~/.config/xfce4-caffeine-plugin/icons/`,
  without overwriting any icon file you've already customized there

Then restart the panel so it picks up the new plugin:

```bash
xfce4-panel -r
```

Now right-click the panel → **Panel** → **Add New Items…** → search for
**"Caffeine"** → Add.

## Custom Icons

Caffeine's panel icon can be replaced with your own artwork. Nothing to
configure beyond picking a theme in Preferences (see below) — just drop
PNG files in the right folder with the right names and the plugin picks
them up automatically the next time it draws (resize the panel, or
restart it with `xfce4-panel -r`, to force a reload). If a file is
missing, Caffeine falls back to its built-in Cairo-drawn cup for that
state, so a partial icon set never breaks anything.

Icons come in **light** and **dark** variants, so your artwork can look
right on both light and dark panels/themes. Which variant gets loaded is
controlled by the **Custom icon theme** setting in Preferences — see
[Preferences](#preferences) below.

**Folder:** `~/.config/xfce4-caffeine-plugin/icons/`

**Files:**

| File | State | Notes |
|---|---|---|
| `off-light.png` | Caffeine OFF, light variant | single static image |
| `off-dark.png` | Caffeine OFF, dark variant | single static image |
| `on-light-01.png` | Caffeine ON, light variant | first (or only) animation frame |
| `on-light-02.png`, `on-light-03.png`, ... | Caffeine ON, light variant | additional animation frames, played in order and looped |
| `on-dark-01.png` | Caffeine ON, dark variant | first (or only) animation frame |
| `on-dark-02.png`, `on-dark-03.png`, ... | Caffeine ON, dark variant | additional animation frames, played in order and looped |

Frame numbers must be zero-padded to 2 digits (`on-light-01.png`, not
`on-light-1.png`) and consecutive with no gaps — loading stops at the
first missing number. `on-light-01.png` alone is fine if you don't want
animation (same for `on-dark-01.png`).

You don't have to provide both variants — if, say, only `off-dark.png`
and `on-dark-*.png` exist, Caffeine just falls back to its built-in cup
whenever the light variant would otherwise be used.

**Format:** PNG, with transparency (alpha channel) if you want the icon
to blend into the panel background rather than showing a solid square.
Animated GIF is *not* supported directly — the plugin instead builds
its own animation by cycling through your `on-light-NN.png` /
`on-dark-NN.png` frames at ~16 fps (the same rate the built-in steam
animation uses), so if you're animating in a GIF editor, just export
each frame as a separate PNG into this folder.

**Size:** the plugin loads and scales each PNG to exactly match the
panel's icon area in pixels, so any source size works, but for the
sharpest result **export at 64×64px**. Panel icon areas typically land
somewhere in the 22–48px range depending on panel size/DPI, so 64px
gives headroom above all of them — downscaling a larger source stays
sharp, while upscaling a smaller one gets blurry. Non-square source
images get squashed to fit, so keep them square.

## Preferences

Right-click the plugin in the panel → **Properties** to open Caffeine's
preferences dialog.

**Lock cycle interval:** while Caffeine is ON, it can lock the screen
(`xflock4`) and blank the monitor (DPMS off, ~7s later) on a repeating
schedule of its own — 15 / 30 / 60 minutes, or a custom number of
minutes — then keep going. This is independent of how long Caffeine
stays on: it doesn't turn Caffeine off, and the screensaver/DPMS inhibit
stays in effect the whole time. Set it to **Never** (the default) for
the original behaviour: stay awake indefinitely with no self-triggered
locking.

**Custom icon theme:** which variant of your custom icons (see [Custom
Icons](#custom-icons) above) gets loaded:

- **Auto** (default) — follows your system/GTK theme automatically
  (technically, the `gtk-application-prefer-dark-theme` setting), so you
  don't have to pick manually or keep it in sync yourself.
- **Light** — always use the `-light` files, regardless of system theme.
- **Dark** — always use the `-dark` files, regardless of system theme.

Hovering over any of these options shows a tip: *"It's recommended to
pick the theme that matches your system."* — Auto already does this for
you, so Light/Dark are mainly there for when you want to override it (or
when you've only prepared one variant of your icon set).

Settings are stored per plugin instance via `xfconf`, so multiple copies
of the plugin (e.g. on different panels) keep independent settings.

## Uninstall

```bash
cd panel-plugin
make uninstall
xfce4-panel -r
```

This removes the plugin binary and `.desktop` file, but deliberately
leaves `~/.config/xfce4-caffeine-plugin/icons/` in place — it may
contain icons you customized. Delete it yourself if you want a clean
slate: `rm -rf ~/.config/xfce4-caffeine-plugin`

## Notes / next steps

- The cup and steam are drawn procedurally with Cairo as the built-in
  fallback (no image files needed for the default look).
- If `Inhibit` fails (e.g. no screensaver DBus service running), the
  plugin will not switch to the "on" visual state and will show an
  error tooltip instead of silently pretending it worked.
- Preferences (lock-cycle interval) live in `caffeine-prefs.c` /
  `caffeine-prefs.h`; custom icon loading lives in `caffeine-icons.c` /
  `caffeine-icons.h` — both kept separate from `caffeine.c` so the core
  inhibit/lifecycle logic stays uncluttered.
- Ideas for later: auto re-enable caffeine on system suspend/resume,
  a file picker for icons in the Properties dialog instead of the
  fixed folder, more preferences (custom lock/blank commands),
  localize the preferences UI (English-only for now).