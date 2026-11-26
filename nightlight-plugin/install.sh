#!/usr/bin/env bash
#
# Night Light -- install / uninstall script
# (Arch Linux, Debian/Ubuntu, Fedora -- XFCE4)
#
#   ./install.sh              installs the plugin
#   ./install.sh uninstall    removes it   (./uninstall.sh does the same)
#
# The plugin is installed per user (~/.local), so no root is needed. sudo is
# only asked for when installing dependencies with the system package
# manager (pacman, apt or dnf).
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

PLUGIN_NAME="nightlight"
DISPLAY_NAME="Night Light"

# Must match the per-user paths used by "make install" (see the Makefile).
PREFIX="$HOME/.local"
PLUGIN_SO="$PREFIX/lib/xfce4/panel/plugins/lib${PLUGIN_NAME}.so"
PLUGIN_DESKTOP="$PREFIX/share/xfce4/panel/plugins/${PLUGIN_NAME}.desktop"
# Translations are installed as <locale dir>/<lang>/LC_MESSAGES/nightlight.mo
LOCALE_DIR="$PREFIX/share/locale"

# Filled in by detect_distro / set_distro_packages.
DISTRO_FAMILY=""   # arch | debian | fedora
DISTRO_NAME=""     # human readable name, for the banner
BUILD_DEPS=()      # needed to build the plugin
OPTIONAL_DEPS=()   # only warned about when missing: "package:what it is needed for"

# Where the panel keeps this plugin's settings (the user is asked before they
# are deleted). The panel names the file nightlight-<id>.rc.
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"

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

# Reads the plugin version from nightlight.h, if it defines NIGHTLIGHT_VERSION
# (e.g.  #define NIGHTLIGHT_VERSION "0.1").  Prints nothing otherwise.
plugin_version() {
    sed -n 's/.*NIGHTLIGHT_VERSION[[:space:]]*"\(.*\)".*/\1/p' \
        panel-plugin/nightlight.h 2>/dev/null | head -n1 || true
}

banner() {
    local version
    version="$(plugin_version)"
    printf '\n%s  %s%s%s\n' "$BOLD$CYAN" "$DISPLAY_NAME" "${version:+  v$version}" "$RESET"
    printf '  %sBlue light reduction panel plugin for XFCE4%s\n\n' "$BLUE" "$RESET"
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
# BUILD_DEPS matches the PKGS list in the Makefile, plus gettext for
# msgfmt, which compiles the translations. The same libraries are needed
# while the plugin is running (the -dev/-devel packages pull them in).
set_distro_packages() {
    case "$DISTRO_FAMILY" in
        arch)
            BUILD_DEPS=(make gcc pkgconf gettext gtk3 xfce4-panel libxfce4util
                        libx11 libxrandr)
            ;;
        debian)
            BUILD_DEPS=(make gcc pkg-config gettext libgtk-3-dev
                        libxfce4panel-2.0-dev libxfce4util-dev libx11-dev
                        libxrandr-dev)
            ;;
        fedora)
            BUILD_DEPS=(make gcc pkgconf-pkg-config gettext gtk3-devel
                        xfce4-panel-devel libxfce4util-devel libX11-devel
                        libXrandr-devel)
            ;;
        *)
            die "Unsupported distribution (${DISTRO_NAME:-unknown}). Supported: Arch Linux, Debian/Ubuntu, Fedora (and their derivatives)."
            ;;
    esac

    # Only warned about when missing, never installed automatically. The
    # package name is the same on all supported distributions.
    OPTIONAL_DEPS=(
        "adwaita-icon-theme:panel icon (weather-clear symbolic icons)"
    )
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
    mapfile -t missing < <(missing_packages "${BUILD_DEPS[@]}")

    if (( ${#missing[@]} == 0 )); then
        ok "Dependencies are already installed!"
    else
        warn "Missing dependencies: ${missing[*]}"
        info "Installing the missing ones (you may be asked for your sudo password)..."
        command -v sudo >/dev/null || die "sudo not found. Install these by hand: ${missing[*]}"
        install_packages "${missing[@]}" || die "Could not install the dependencies."
        ok "Dependencies installed."
    fi

    # Packages that are not installed automatically, but whose absence
    # may mean a part of the plugin won't work.
    local entry pkg what
    for entry in "${OPTIONAL_DEPS[@]}"; do
        pkg="${entry%%:*}"
        what="${entry#*:}"
        if [[ -n "$(missing_packages "$pkg")" ]]; then
            warn "$pkg is not installed, so the $what may not show up. ($(install_hint "$pkg"))"
        fi
    done
}

do_install() {
    detect_distro
    set_distro_packages
    [[ -f Makefile ]] || die "Makefile not found. Run this script from the project's main folder."

    banner
    info "Detected system: $DISTRO_NAME ($DISTRO_FAMILY)"
    echo

    if ! ask "Install $DISPLAY_NAME? Are you sure?"; then
        abort
    fi

    if is_wayland; then
        echo
        warn "Wayland is not supported at the moment (the screen color is set through XRandR, which needs X11)."
        if ! ask "Do you want to install it anyway?"; then
            abort
        fi
    fi

    install_dependencies

    step "Building and installing (make install)..."
    make -j"$(nproc)" install || die "Build or installation failed."
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
    printf '  %s2.%s Pick %s%s%s from the list (shown in your system language)\n' \
        "$CYAN" "$RESET" "$BOLD" "$DISPLAY_NAME" "$RESET"
    printf '  %s3.%s Click %sAdd%s\n\n' "$CYAN" "$RESET" "$BOLD" "$RESET"
    printf 'Click the plugin icon to turn the night light on or off.\n'
    printf 'For its settings: right-click the plugin → %sProperties%s\n\n' "$BOLD" "$RESET"
}

# --- uninstall ----------------------------------------------------------------------

# Prints the installed translation catalogs, one per line.
installed_catalogs() {
    local m
    shopt -s nullglob
    for m in "$LOCALE_DIR"/*/LC_MESSAGES/"${PLUGIN_NAME}.mo"; do
        printf '%s\n' "$m"
    done
    shopt -u nullglob
}

# Prints the settings/data paths that currently exist, one per line.
existing_data_paths() {
    local p
    shopt -s nullglob
    for p in "$CONFIG_HOME"/xfce4/panel/"${PLUGIN_NAME}"-*.rc
    do
        [[ -e "$p" ]] && printf '%s\n' "$p"
    done
    shopt -u nullglob
}

do_uninstall() {
    banner

    local found=() catalogs=()
    [[ -e "$PLUGIN_SO" ]]      && found+=("$PLUGIN_SO")
    [[ -e "$PLUGIN_DESKTOP" ]] && found+=("$PLUGIN_DESKTOP")
    mapfile -t catalogs < <(installed_catalogs)

    if (( ${#found[@]} == 0 && ${#catalogs[@]} == 0 )); then
        warn "$DISPLAY_NAME doesn't seem to be installed (nothing found under $PREFIX)."
        exit 0
    fi

    ok "$DISPLAY_NAME found:"
    local f
    for f in "${found[@]}"; do
        printf '    %s\n' "$f"
    done
    if (( ${#catalogs[@]} > 0 )); then
        printf '    %s/share/locale/<lang>/LC_MESSAGES/%s.mo  (%d languages)\n' \
            "$PREFIX" "$PLUGIN_NAME" "${#catalogs[@]}"
    fi
    echo

    if ! ask "Remove $DISPLAY_NAME?"; then
        abort
    fi

    rm -f -- "$PLUGIN_SO" "$PLUGIN_DESKTOP"
    for f in "${catalogs[@]}"; do
        rm -f -- "$f"
        # drop the language folders if this was the last file in them
        rmdir --ignore-fail-on-non-empty -- "$(dirname "$f")" "$(dirname "$(dirname "$f")")" 2>/dev/null || true
    done
    ok "$DISPLAY_NAME removed."

    echo
    local data=()
    mapfile -t data < <(existing_data_paths)
    if (( ${#data[@]} > 0 )); then
        info "Settings/data found:"
        for f in "${data[@]}"; do
            printf '    %s\n' "$f"
        done
    fi

    if ask "Also delete $DISPLAY_NAME's data and settings?"; then
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