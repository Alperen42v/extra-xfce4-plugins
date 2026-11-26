#!/usr/bin/env bash
#
# Extras Menu -- install / uninstall script
# (Arch Linux, Debian/Ubuntu, Fedora -- XFCE4)
#
#   ./install.sh              installs the plugin
#   ./install.sh uninstall    removes it   (./uninstall.sh does the same)
#
# The plugin is installed per user (default: ~/.local), so no root is
# needed. sudo is only asked for when installing dependencies with the
# system package manager (pacman, apt or dnf).
# To install somewhere else: PREFIX=/usr/local ./install.sh
#
# Supported distributions (derivatives are detected through ID_LIKE in
# /etc/os-release, e.g. Manjaro, Linux Mint, Pop!_OS, Rocky):
#   Arch Linux            -> pacman
#   Debian / Ubuntu       -> apt
#   Fedora                -> dnf

set -euo pipefail

# Always work from the project folder, wherever the script is called from.
cd "$(dirname "$(readlink -f "$0")")"

# --- settings -----------------------------------------------------------------

PLUGIN_NAME="extras-menu"
PREFIX="${PREFIX:-$HOME/.local}"
PLUGIN_SO="$PREFIX/lib/xfce4/panel/plugins/lib${PLUGIN_NAME}.so"
PLUGIN_DESKTOP="$PREFIX/share/xfce4/panel/plugins/${PLUGIN_NAME}.desktop"

# Filled in by detect_distro / set_distro_packages.
DISTRO_FAMILY=""      # arch | debian | fedora
DISTRO_NAME=""        # human readable name, for the banner
BUILD_DEPS=()         # needed to build the plugin (matches the PKGS list in the Makefile)
RUNTIME_DEPS=()       # needed while the plugin is running
OPTIONAL_DEPS=()      # only warned about when missing: "package:what it is needed for"
SOUND_SERVER_PKGS=()  # any one of these is enough for the volume sliders

# Where the plugin keeps its settings (the user is asked before these are
# deleted). Update this if the plugin stores its settings somewhere else.
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XFCONF_CHANNEL="extras-menu"

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

# Reads the plugin version from preferences.h, if it can be found.
plugin_version() {
    sed -n 's/.*EXTRAS_MENU_VERSION[[:space:]]*"\(.*\)".*/\1/p' \
        panel-plugin/preferences/preferences.h 2>/dev/null | head -n1
}

banner() {
    local version
    version="$(plugin_version)"
    printf '\n%s  Extras Menu%s%s\n' "$BOLD$CYAN" "${version:+  v$version}" "$RESET"
    printf '  %sPanel plugin for XFCE4%s\n\n' "$BLUE" "$RESET"
}

# --- distribution support ---------------------------------------------------------

# Finds out which package manager family this system uses, first from
# /etc/os-release (ID, then ID_LIKE, so derivatives work too), then, as a
# fallback, from the package manager that is installed.
detect_distro() {
    local id="" id_like="" pretty="" token

    if [[ -r /etc/os-release ]]; then
        # Read in a subshell so the os-release variables don't leak into
        # this script (it prints exactly three lines, empty ones included).
        {
            read -r id
            read -r id_like
            read -r pretty
        } < <(. /etc/os-release; printf '%s\n%s\n%s\n' "${ID:-}" "${ID_LIKE:-}" "${PRETTY_NAME:-}") || true
    fi

    DISTRO_FAMILY=""
    # shellcheck disable=SC2086  # word splitting of ID_LIKE is intended
    for token in $id $id_like; do
        case "$token" in
            arch)             DISTRO_FAMILY="arch" ;;
            debian|ubuntu)    DISTRO_FAMILY="debian" ;;
            fedora)           DISTRO_FAMILY="fedora" ;;
        esac
        [[ -z "$DISTRO_FAMILY" ]] || break
    done

    if [[ -z "$DISTRO_FAMILY" ]]; then
        if   command -v pacman  >/dev/null; then DISTRO_FAMILY="arch"
        elif command -v apt-get >/dev/null; then DISTRO_FAMILY="debian"
        elif command -v dnf     >/dev/null; then DISTRO_FAMILY="fedora"
        fi
    fi

    DISTRO_NAME="${pretty:-${id:-unknown}}"
}

# Sets the package lists. Package names differ between distributions
# (e.g. gtk3 / libgtk-3-dev / gtk3-devel), so each family has its own.
set_distro_packages() {
    case "$DISTRO_FAMILY" in
        arch)
            BUILD_DEPS=(make gcc pkgconf gtk3 xfce4-panel libxfce4ui libxfce4util
                        xfconf libpulse glib2)
            RUNTIME_DEPS=(brightnessctl power-profiles-daemon)
            OPTIONAL_DEPS=(
                "networkmanager:Wi-Fi and Ethernet"
                "bluez:Bluetooth"
                "upower:battery indicator"
            )
            SOUND_SERVER_PKGS=(pipewire-pulse pulseaudio)
            ;;
        debian)
            BUILD_DEPS=(make gcc pkg-config libgtk-3-dev libxfce4panel-2.0-dev
                        libxfce4ui-2-dev libxfce4util-dev libxfconf-0-dev
                        libpulse-dev libglib2.0-dev)
            RUNTIME_DEPS=(brightnessctl power-profiles-daemon)
            OPTIONAL_DEPS=(
                "network-manager:Wi-Fi and Ethernet"
                "bluez:Bluetooth"
                "upower:battery indicator"
            )
            SOUND_SERVER_PKGS=(pipewire-pulse pulseaudio)
            ;;
        fedora)
            BUILD_DEPS=(make gcc pkgconf-pkg-config gtk3-devel xfce4-panel-devel
                        libxfce4ui-devel libxfce4util-devel xfconf-devel
                        pulseaudio-libs-devel glib2-devel)
            RUNTIME_DEPS=(brightnessctl power-profiles-daemon)
            OPTIONAL_DEPS=(
                "NetworkManager:Wi-Fi and Ethernet"
                "bluez:Bluetooth"
                "upower:battery indicator"
            )
            SOUND_SERVER_PKGS=(pipewire-pulseaudio pulseaudio)
            ;;
        *)
            die "Unsupported distribution (${DISTRO_NAME:-unknown}). Supported: Arch Linux, Debian/Ubuntu, Fedora (and their derivatives)."
            ;;
    esac
}

# missing_packages pkg... -- prints the given packages that are NOT
# installed, one per line.
missing_packages() {
    local p
    case "$DISTRO_FAMILY" in
        arch)
            # pacman -T lists the packages that are NOT installed (it also
            # takes "provides" into account, so e.g. a replacement package
            # counts). It exits with 127 when something is missing.
            pacman -T "$@" || true
            ;;
        debian)
            for p in "$@"; do
                if [[ "$(dpkg-query -W -f='${db:Status-Status}' "$p" 2>/dev/null || true)" != "installed" ]]; then
                    printf '%s\n' "$p"
                fi
            done
            ;;
        fedora)
            for p in "$@"; do
                # --whatprovides also accepts package names and "provides"
                # (so e.g. tuned-ppd counts for power-profiles-daemon)
                if ! rpm -q --quiet --whatprovides "$p"; then
                    printf '%s\n' "$p"
                fi
            done
            ;;
    esac
}

# install_packages pkg... -- installs packages with the system package
# manager (asks for the sudo password; the package manager asks for
# confirmation itself).
install_packages() {
    case "$DISTRO_FAMILY" in
        arch)   sudo pacman -S --needed "$@" ;;
        debian) sudo apt-get update && sudo apt-get install "$@" ;;
        fedora) sudo dnf install "$@" ;;
    esac
}

# install_hint pkg -- the command a user can run to install one package
# by hand (used in warnings).
install_hint() {
    case "$DISTRO_FAMILY" in
        arch)   printf 'sudo pacman -S %s' "$1" ;;
        debian) printf 'sudo apt install %s' "$1" ;;
        fedora) printf 'sudo dnf install %s' "$1" ;;
    esac
}

# --- install ----------------------------------------------------------------------

is_wayland() {
    [[ "${XDG_SESSION_TYPE:-}" == "wayland" || -n "${WAYLAND_DISPLAY:-}" ]]
}

install_dependencies() {
    step "Checking dependencies..."

    local missing=()
    mapfile -t missing < <(missing_packages "${BUILD_DEPS[@]}" "${RUNTIME_DEPS[@]}")

    if (( ${#missing[@]} == 0 )); then
        ok "Dependencies are already installed!"
    else
        warn "Missing dependencies: ${missing[*]}"
        info "Installing the missing ones (you may be asked for your sudo password)..."
        command -v sudo >/dev/null || die "sudo not found. Install these by hand: ${missing[*]}"
        install_packages "${missing[@]}" || die "Could not install the dependencies."
        ok "Dependencies installed."

        # The Balanced pill talks to the power-profiles-daemon service.
        if printf '%s\n' "${missing[@]}" | grep -qx 'power-profiles-daemon'; then
            info "Enabling the power-profiles-daemon service..."
            sudo systemctl enable --now power-profiles-daemon.service \
                || warn "Could not start the service. Try: sudo systemctl enable --now power-profiles-daemon"
        fi
    fi

    # Packages that are not installed automatically, but whose absence
    # means a feature of the plugin won't work.
    local entry pkg what
    for entry in "${OPTIONAL_DEPS[@]}"; do
        pkg="${entry%%:*}"
        what="${entry#*:}"
        if [[ -n "$(missing_packages "$pkg")" ]]; then
            warn "$pkg is not installed, so the $what won't work. ($(install_hint "$pkg"))"
        fi
    done

    # libpulse is only the client library; a sound server is needed too.
    local have_sound=0 s
    for s in "${SOUND_SERVER_PKGS[@]}"; do
        if [[ -z "$(missing_packages "$s")" ]]; then
            have_sound=1
            break
        fi
    done
    if (( ! have_sound )); then
        warn "Neither ${SOUND_SERVER_PKGS[0]} nor ${SOUND_SERVER_PKGS[1]} is installed, so the volume sliders won't work."
    fi
}

do_install() {
    detect_distro
    set_distro_packages
    [[ -f Makefile ]] || die "Makefile not found. Run this script from the project's main folder."

    banner
    info "Detected system: $DISTRO_NAME ($DISTRO_FAMILY)"
    echo

    if ! ask "Install Extras Menu? Are you sure?"; then
        abort
    fi

    if is_wayland; then
        echo
        warn "Wayland is not supported at the moment."
        if ! ask "Do you want to install it anyway?"; then
            abort
        fi
    fi

    install_dependencies

    step "Building and installing (make install)..."
    make -j"$(nproc)" install PREFIX="$PREFIX" || die "Build or installation failed."
    ok "Plugin installed: $PLUGIN_SO"

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
    printf '  %s2.%s Pick %sExtras Menu%s from the list\n' "$CYAN" "$RESET" "$BOLD" "$RESET"
    printf '  %s3.%s Click %sAdd%s\n\n' "$CYAN" "$RESET" "$BOLD" "$RESET"
    printf 'For its settings: right-click the plugin → %sProperties%s\n\n' "$BOLD" "$RESET"
}

# --- uninstall ----------------------------------------------------------------------

# Prints the settings/data paths that currently exist, one per line.
existing_data_paths() {
    local p
    shopt -s nullglob
    for p in \
        "$CONFIG_HOME"/xfce4/panel/${PLUGIN_NAME}*.rc \
        "$CONFIG_HOME/xfce4/xfconf/xfce-perchannel-xml/${PLUGIN_NAME}.xml" \
        "$CONFIG_HOME/${PLUGIN_NAME}"
    do
        [[ -e "$p" ]] && printf '%s\n' "$p"
    done
    shopt -u nullglob
}

do_uninstall() {
    banner

    local found=()
    [[ -e "$PLUGIN_SO" ]]      && found+=("$PLUGIN_SO")
    [[ -e "$PLUGIN_DESKTOP" ]] && found+=("$PLUGIN_DESKTOP")

    if (( ${#found[@]} == 0 )); then
        warn "Extras Menu doesn't seem to be installed (nothing found under $PREFIX)."
        exit 0
    fi

    ok "Extras Menu found:"
    local f
    for f in "${found[@]}"; do
        printf '    %s\n' "$f"
    done
    echo

    if ! ask "Remove Extras Menu?"; then
        abort
    fi

    rm -f -- "$PLUGIN_SO" "$PLUGIN_DESKTOP"
    ok "Extras Menu removed."

    echo
    local data=()
    mapfile -t data < <(existing_data_paths)
    if (( ${#data[@]} > 0 )); then
        info "Settings/data found:"
        for f in "${data[@]}"; do
            printf '    %s\n' "$f"
        done
    fi

    if ask "Also delete Extras Menu's data and settings?"; then
        # If the plugin keeps settings in xfconf, reset those too.
        if command -v xfconf-query >/dev/null \
            && xfconf-query -c "$XFCONF_CHANNEL" -l >/dev/null 2>&1; then
            xfconf-query -c "$XFCONF_CHANNEL" -p / -r -R >/dev/null 2>&1 || true
            ok "xfconf settings ($XFCONF_CHANNEL) reset."
        fi

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

# The plugin goes into the user's own home; running as root would put it
# into root's home instead. sudo is called by the script where needed.
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