#!/usr/bin/env bash
#
# Caffeine -- install / uninstall script (Arch Linux, XFCE4)
#
#   ./install.sh              installs the plugin
#   ./install.sh uninstall    removes it   (./uninstall.sh does the same)
#
# The plugin is installed per user (under ~/.local), so no root is
# needed. sudo is only asked for when installing dependencies with pacman.
# For a system-wide install, run `sudo make install` yourself instead.

set -euo pipefail

# Always work from the project folder, wherever the script is called from.
cd "$(dirname "$(readlink -f "$0")")"

# --- settings -----------------------------------------------------------------

# These paths mirror what the Makefile installs for a non-root user.
PLUGIN_NAME="caffeine"
GETTEXT_PACKAGE="xfce4-caffeine-plugin"
PLUGIN_SO="$HOME/.local/lib/xfce4/panel/plugins/lib${PLUGIN_NAME}.so"
PLUGIN_DESKTOP="$HOME/.local/share/xfce4/panel/plugins/caffeine-plugin.desktop"
LOCALE_DIR="$HOME/.local/share/locale"
USER_ICONS_ROOT="$HOME/.config/xfce4-caffeine-plugin"   # holds the icons/ folder

# Needed to build the plugin (matches the pkg-config list in the Makefile;
# gettext provides msgfmt for the translations).
BUILD_DEPS=(make gcc pkgconf gtk3 xfce4-panel xfconf glib2 gettext)

# Needed while the plugin is running: xflock4 (lock cycle) and xset
# (screen-off timer).
RUNTIME_DEPS=(xfce4-session xorg-xset)

# Only warned about when missing, never installed automatically (so the
# script can't disturb the power/session setup of the system):
# "package:what it is needed for".
OPTIONAL_DEPS=(
    "xfce4-power-manager:keeping the monitor awake (power inhibit)"
)

# Where the plugin keeps its settings: one xfconf channel per plugin
# instance, named "<prefix><id>" (the user is asked before these are deleted).
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XFCONF_CHANNEL_PREFIX="xfce4-caffeine-plugin-"

# --- colors and helpers ---------------------------------------------------------

# Colors only when writing to a terminal, so logs and pipes stay clean.
if [[ -t 1 ]]; then
    RED=$'\e[1;31m'; GREEN=$'\e[1;32m'; YELLOW=$'\e[1;33m'
    BLUE=$'\e[1;34m'; CYAN=$'\e[1;36m'; BOLD=$'\e[1m'; RESET=$'\e[0m'
else
    RED=""; GREEN=""; YELLOW=""; BLUE=""; CYAN=""; BOLD=""; RESET=""
fi

info()  { printf '%s::%s %s\n'  "$BLUE"   "$RESET" "$*"; }
step()  { printf '\n%s==>%s %s%s%s\n' "$CYAN" "$RESET" "$BOLD" "$*" "$RESET"; }
ok()    { printf '%s✔%s %s\n'   "$GREEN"  "$RESET" "$*"; }
warn()  { printf '%s⚠ Warning:%s %s\n' "$YELLOW" "$RESET" "$*"; }
fail()  { printf '%s✘%s %s\n'   "$RED"    "$RESET" "$*" >&2; }

# Prints an error and stops.
die() {
    fail "$*"
    exit 1
}

# The user answered "no" to a question: stop without doing anything more.
abort() {
    printf '\n%sAborted.%s\n' "$YELLOW" "$RESET"
    exit 1
}

# ask "question" -- succeeds on Y/y (or "yes"); everything else (N, an
# empty answer, no input at all) counts as "no".
ask() {
    local reply=""
    printf '%s%s%s [Y/N]: ' "$BOLD" "$1" "$RESET"
    read -r reply || reply=""
    [[ "$reply" =~ ^([yY]|[yY][eE][sS])$ ]]
}

banner() {
    printf '\n%s  Caffeine%s\n' "$BOLD$CYAN" "$RESET"
    printf '  %sPanel plugin for XFCE4 - keep your screen awake%s\n\n' "$BLUE" "$RESET"
}

# Prints the installed translation files (.mo), one per line.
translation_files() {
    local f
    shopt -s nullglob
    for f in "$LOCALE_DIR"/*/LC_MESSAGES/"$GETTEXT_PACKAGE".mo; do
        printf '%s\n' "$f"
    done
    shopt -u nullglob
}

# Warns if a system-wide copy (from `sudo make install`) also exists: it
# is not touched by this per-user script.
warn_system_copy() {
    local libdir system_so
    libdir="$(pkg-config --variable=libdir libxfce4panel-2.0 2>/dev/null || true)"
    [[ -n "$libdir" ]] || return 0
    system_so="$libdir/xfce4/panel/plugins/lib${PLUGIN_NAME}.so"
    if [[ -e "$system_so" ]]; then
        warn "A system-wide copy also exists: $system_so"
        info "This script only handles the per-user copy. Remove the system one with: sudo make uninstall"
    fi
}

# --- install ----------------------------------------------------------------------

# The lock cycle and the screen-off timer use X11 tools (xflock4, xset).
is_wayland() {
    [[ "${XDG_SESSION_TYPE:-}" == "wayland" || -n "${WAYLAND_DISPLAY:-}" ]]
}

install_dependencies() {
    step "Checking dependencies..."

    # pacman -T lists the packages that are NOT installed (it also takes
    # "provides" into account, so e.g. a replacement package counts).
    local missing=()
    mapfile -t missing < <(pacman -T "${BUILD_DEPS[@]}" "${RUNTIME_DEPS[@]}" || true)

    if (( ${#missing[@]} == 0 )); then
        ok "Dependencies are already installed!"
    else
        warn "Missing dependencies: ${missing[*]}"
        info "Installing the missing ones (you may be asked for your sudo password)..."
        command -v sudo >/dev/null || die "sudo not found. Install these by hand: ${missing[*]}"
        sudo pacman -S --needed "${missing[@]}" || die "Could not install the dependencies."
        ok "Dependencies installed."
    fi

    # Packages that are not installed automatically, but whose absence
    # means a feature of the plugin won't work.
    local entry pkg what
    for entry in "${OPTIONAL_DEPS[@]}"; do
        pkg="${entry%%:*}"
        what="${entry#*:}"
        if [[ -n "$(pacman -T "$pkg" || true)" ]]; then
            warn "$pkg is not installed, so $what may not work. (sudo pacman -S $pkg)"
        fi
    done

    # Caffeine inhibits the screensaver over D-Bus, so one of these
    # has to be running for that half to have any effect.
    if [[ -n "$(pacman -T xfce4-screensaver || true)" \
       && -n "$(pacman -T xscreensaver || true)" \
       && -n "$(pacman -T light-locker || true)" ]]; then
        warn "No screensaver/locker (xfce4-screensaver, xscreensaver, light-locker) is installed, so there is nothing for Caffeine to inhibit."
    fi
}

do_install() {
    command -v pacman >/dev/null || die "This script is written for Arch Linux (pacman)."
    [[ -f Makefile && -f caffeine-plugin.desktop ]] \
        || die "Makefile or caffeine-plugin.desktop not found. Run this script from the project's main folder."

    banner

    if ! ask "Install Caffeine? Are you sure?"; then
        abort
    fi

    if is_wayland; then
        echo
        warn "Wayland is not supported at the moment (the lock cycle and screen-off timer use X11 tools)."
        if ! ask "Do you want to install it anyway?"; then
            abort
        fi
    fi

    install_dependencies

    step "Building and installing (make install)..."
    make -j"$(nproc)" install || die "Build or installation failed."
    [[ -f "$PLUGIN_SO" ]] || die "Build finished but $PLUGIN_SO is missing."
    ok "Plugin installed: $PLUGIN_SO"

    # All languages in po/ are installed together; the plugin picks the
    # one matching the system language each time it starts.
    local langs
    langs="$(translation_files | wc -l)"
    if (( langs > 0 )); then
        ok "Translations installed: $langs language(s)."
    else
        warn "No translations were installed. Is the po/ folder next to the Makefile?"
    fi

    warn_system_copy

    step "Restarting the panel (xfce4-panel -r)..."
    if command -v xfce4-panel >/dev/null && xfce4-panel -r >/dev/null 2>&1; then
        ok "Panel restarted."
    else
        warn "Could not restart the panel. Inside your XFCE session run: xfce4-panel -r"
    fi

    printf '\n%s🎉 Installation complete!%s\n\n' "$GREEN" "$RESET"
    printf '%sTo add it to your panel:%s\n' "$BOLD" "$RESET"
    printf '  %s1.%s Right-click the panel → %sPanel%s → %sAdd New Items...%s\n' \
        "$CYAN" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET"
    printf '  %s2.%s Pick %sCaffeine%s from the list\n' "$CYAN" "$RESET" "$BOLD" "$RESET"
    printf '  %s3.%s Click %sAdd%s\n\n' "$CYAN" "$RESET" "$BOLD" "$RESET"
    printf 'For its settings: right-click the plugin → %sProperties%s\n' "$BOLD" "$RESET"
    printf 'The language follows your system language; after changing it, run: xfce4-panel -r\n\n'
}

# --- uninstall ----------------------------------------------------------------------

# Prints the xfconf channels the plugin created (one per plugin instance).
xfconf_channels() {
    command -v xfconf-query >/dev/null || return 0
    xfconf-query -l 2>/dev/null \
        | sed -n "s/^[[:space:]]*\\(${XFCONF_CHANNEL_PREFIX}.*\\)\$/\\1/p" || true
}

# Prints the settings/data paths that currently exist, one per line.
existing_data_paths() {
    local p
    shopt -s nullglob
    for p in \
        "$CONFIG_HOME"/xfce4/xfconf/xfce-perchannel-xml/"${XFCONF_CHANNEL_PREFIX}"*.xml \
        "$USER_ICONS_ROOT"
    do
        if [[ -e "$p" ]]; then
            printf '%s\n' "$p"
        fi
    done
    shopt -u nullglob
}

do_uninstall() {
    banner

    local found=() f
    if [[ -e "$PLUGIN_SO" ]]; then found+=("$PLUGIN_SO"); fi
    if [[ -e "$PLUGIN_DESKTOP" ]]; then found+=("$PLUGIN_DESKTOP"); fi
    mapfile -t -O "${#found[@]}" found < <(translation_files)

    if (( ${#found[@]} == 0 )); then
        warn "Caffeine doesn't seem to be installed for this user (nothing found under ~/.local)."
        warn_system_copy
        exit 0
    fi

    ok "Caffeine found:"
    for f in "${found[@]}"; do
        printf '    %s\n' "$f"
    done
    echo

    if ! ask "Remove Caffeine?"; then
        abort
    fi

    rm -f -- "$PLUGIN_SO" "$PLUGIN_DESKTOP"
    for f in "${found[@]}"; do
        if [[ "$f" == *.mo ]]; then
            rm -f -- "$f"
            # tidy up the language folders if they are now empty
            rmdir --ignore-fail-on-non-empty -- "$(dirname "$f")" "$(dirname "$(dirname "$f")")" 2>/dev/null || true
        fi
    done
    ok "Caffeine removed."

    warn_system_copy

    echo
    local data=()
    mapfile -t data < <(existing_data_paths)
    if (( ${#data[@]} > 0 )); then
        info "Settings/data found:"
        for f in "${data[@]}"; do
            printf '    %s\n' "$f"
        done
        info "Note: the icons folder may contain PNG icons you added yourself."
    fi

    if ask "Also delete Caffeine's data and settings?"; then
        # Reset the xfconf channels first: xfconfd rewrites the files
        # from memory, so deleting only the files could bring them back.
        local ch
        while IFS= read -r ch; do
            [[ -n "$ch" ]] || continue
            xfconf-query -c "$ch" -p / -r -R >/dev/null 2>&1 || true
            ok "xfconf settings ($ch) reset."
        done < <(xfconf_channels)

        # Look again: resetting xfconf may have changed what is on disk.
        mapfile -t data < <(existing_data_paths)
        for f in "${data[@]}"; do
            rm -rf -- "$f"
            ok "Deleted: $f"
        done
        ok "Data and settings removed."
    else
        info "Settings kept. If you install again, they will be picked up as they were."
    fi

    printf '\n%sUninstall complete.%s\n' "$GREEN" "$RESET"
    warn "If the plugin is still on your panel, right-click it and choose Remove, then run: xfce4-panel -r"
    echo
}

# --- entry point -----------------------------------------------------------------------

# The plugin goes into the user's own home; running as root would make the
# Makefile install system-wide instead. sudo is called by the script where needed.
if [[ "$EUID" -eq 0 ]]; then
    die "Don't run this as root. The plugin is installed per user, and the script asks for sudo itself when needed."
fi

case "${1:-install}" in
    install)           do_install ;;
    uninstall|remove)  do_uninstall ;;
    -h|--help|help)
        printf 'Usage: %s [install|uninstall]\n' "$0"
        ;;
    *)
        die "Unknown command: $1 (use install or uninstall)"
        ;;
esac