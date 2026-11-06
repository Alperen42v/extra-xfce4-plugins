#!/usr/bin/env bash
# Thin wrapper: same as "./install.sh uninstall".
exec "$(dirname "$(readlink -f "$0")")/install.sh" uninstall