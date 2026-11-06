#!/usr/bin/env bash
# Shortcut for:  ./install.sh uninstall
exec bash "$(dirname "$(readlink -f "$0")")/install.sh" uninstall "$@"