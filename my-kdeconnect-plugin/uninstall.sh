#!/usr/bin/env bash
# Same as: ./install.sh uninstall
exec "$(dirname "$(readlink -f "$0")")/install.sh" uninstall "$@"