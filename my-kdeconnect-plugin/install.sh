#!/usr/bin/env bash
#
# My KDE Connect panel plugin -- install / uninstall script
# (Arch Linux, Debian/Ubuntu, Fedora -- XFCE4)
#
#   ./install.sh              installs the plugin
#   ./install.sh uninstall    removes it   (./uninstall.sh does the same)
#
# The plugin is installed per user (under ~/.local), so no root is
# needed. sudo is only asked for when installing dependencies with the
# system package manager (pacman, apt or dnf).
# For a system-wide install, run `sudo make install` yourself instead.
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

# These paths mirror what the Makefile installs for a non-root user.
# (Check them against the install target of your Makefile.)
PLUGIN_NAME="kdeconnect"
PLUGIN_SO="$HOME/.local/lib/xfce4/panel/plugins/lib${PLUGIN_NAME}.so"
PLUGIN_DESKTOP="$HOME/.local/share/xfce4/panel/plugins/kdeconnect-plugin.desktop"

# Filled in by detect_distro / set_distro_packages.
DISTRO_FAMILY=""   # arch | debian | fedora
DISTRO_NAME=""     # human readable name, for the banner
GROUP_DEPS=()      # package groups (Arch only, see set_distro_packages)
DEPS=()            # needed to build and run the plugin

# Where the plugin keeps its settings. Panel plugins usually store them in
# ~/.config/xfce4/panel/<plugin>-<id>.rc, or in an xfconf channel named
# "<prefix><id>". Both are checked; the user is asked before anything is deleted.
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XFCONF_CHANNEL_PREFIX="xfce4-kdeconnect-plugin-"

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
    printf '\n%s  My KDE Connect%s\n' "$BOLD$CYAN" "$RESET"
    printf '  %sPanel plugin for XFCE4 - connect your phone to your desktop%s\n\n' "$BLUE" "$RESET"
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
#
# Arch has the "base-devel" group, which pacman -T cannot check, so its
# members are looked up and checked one by one (see install_dependencies).
# Debian has the build-essential package; Fedora has no single package, so
# the compiler tools are listed one by one.
# kdeconnect is required: the plugin talks to its daemon.
set_distro_packages() {
    case "$DISTRO_FAMILY" in
        arch)
            GROUP_DEPS=(base-devel)
            DEPS=(pkgconf gtk3 xfce4-panel libxfce4util kdeconnect libarchive)
            ;;
        debian)
            GROUP_DEPS=()
            DEPS=(build-essential pkg-config libgtk-3-dev libxfce4panel-2.0-dev
                  libxfce4util-dev kdeconnect libarchive-dev)
            ;;
        fedora)
            GROUP_DEPS=()
            DEPS=(gcc gcc-c++ make pkgconf-pkg-config gtk3-devel xfce4-panel-devel
                  libxfce4util-devel kde-connect libarchive-devel)
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

# --- install ----------------------------------------------------------------------

install_dependencies() {
    step "Checking dependencies..."

    # Expand the groups (Arch: base-devel) into their member packages.
    local group_pkgs=() g
    for g in "${GROUP_DEPS[@]}"; do
        mapfile -t -O "${#group_pkgs[@]}" group_pkgs < <(pacman -Sgq "$g")
    done

    local missing=()
    mapfile -t missing < <(missing_packages "${group_pkgs[@]}" "${DEPS[@]}")

    if (( ${#missing[@]} == 0 )); then
        ok "Dependencies are already installed!"
    else
        warn "Missing dependencies: ${missing[*]}"
        info "Installing the missing ones (you may be asked for your sudo password)..."
        command -v sudo >/dev/null || die "sudo not found. Install these by hand: ${missing[*]}"
        install_packages "${missing[@]}" || die "Could not install the dependencies."
        ok "Dependencies installed."
    fi
}

do_install() {
    detect_distro
    set_distro_packages
    [[ -f Makefile && -f kdeconnect-plugin.desktop && -d panel-plugin ]] \
        || die "Makefile, kdeconnect-plugin.desktop or panel-plugin/ not found. Run this script from the project's main folder."

    banner
    info "Detected system: $DISTRO_NAME ($DISTRO_FAMILY)"
    echo

    if ! ask "Install My KDE Connect panel plugin? Are you sure?"; then
        abort
    fi

    install_dependencies

    step "Building and installing (make install)..."
    make -j"$(nproc)" install || die "Build or installation failed."
    [[ -f "$PLUGIN_SO" ]] || die "Build finished but $PLUGIN_SO is missing."
    [[ -f "$PLUGIN_DESKTOP" ]] || die "Build finished but $PLUGIN_DESKTOP is missing."
    ok "Plugin installed: $PLUGIN_SO"

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
    printf '  %s2.%s Pick %sMy KDE Connect%s from the list\n' "$CYAN" "$RESET" "$BOLD" "$RESET"
    printf '  %s3.%s Click %sAdd%s\n\n' "$CYAN" "$RESET" "$BOLD" "$RESET"
    printf 'For its settings: right-click the plugin → %sProperties%s\n\n' "$BOLD" "$RESET"
}

# --- uninstall ----------------------------------------------------------------------

# Prints the xfconf channels the plugin created (one per plugin instance).
xfconf_channels() {
    command -v xfconf-query >/dev/null || return 0
    xfconf-query -l 2>/dev/null \
        | sed -n "s/^[[:space:]]*\\(${XFCONF_CHANNEL_PREFIX}.*\\)\$/\\1/p" || true
}

# Prints the settings files that currently exist, one per line.
existing_data_paths() {
    local p
    shopt -s nullglob
    for p in \
        "$CONFIG_HOME"/xfce4/panel/"${PLUGIN_NAME}"-*.rc \
        "$CONFIG_HOME"/xfce4/xfconf/xfce-perchannel-xml/"${XFCONF_CHANNEL_PREFIX}"*.xml
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

    if (( ${#found[@]} == 0 )); then
        warn "My KDE Connect plugin doesn't seem to be installed for this user (nothing found under ~/.local)."
        warn_system_copy
        exit 0
    fi

    ok "My KDE Connect plugin found:"
    for f in "${found[@]}"; do
        printf '    %s\n' "$f"
    done
    echo

    if ! ask "Remove My KDE Connect plugin?"; then
        abort
    fi

    rm -f -- "$PLUGIN_SO" "$PLUGIN_DESKTOP"
    ok "My KDE Connect plugin removed."

    warn_system_copy

    echo
    local data=() channels=()
    mapfile -t data < <(existing_data_paths)
    mapfile -t channels < <(xfconf_channels)

    if (( ${#data[@]} > 0 || ${#channels[@]} > 0 )); then
        info "Settings/data found:"
        for f in "${data[@]}"; do
            printf '    %s\n' "$f"
        done
        for f in "${channels[@]}"; do
            printf '    xfconf channel: %s\n' "$f"
        done

        if ask "Also delete the plugin's settings?"; then
            # Reset the xfconf channels first: xfconfd rewrites the files
            # from memory, so deleting only the files could bring them back.
            local ch
            for ch in "${channels[@]}"; do
                [[ -n "$ch" ]] || continue
                xfconf-query -c "$ch" -p / -r -R >/dev/null 2>&1 || true
                ok "xfconf settings ($ch) reset."
            done

            # Look again: resetting xfconf may have changed what is on disk.
            mapfile -t data < <(existing_data_paths)
            for f in "${data[@]}"; do
                rm -rf -- "$f"
                ok "Deleted: $f"
            done
            ok "Settings removed."
        else
            info "Settings kept. If you install again, they will be picked up as they were."
        fi
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