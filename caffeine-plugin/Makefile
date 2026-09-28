PLUGIN_NAME = caffeine

# gettext domain: the .mo files are installed under this name
GETTEXT_PACKAGE = xfce4-caffeine-plugin

PKG_CONFIG = pkg-config

# Not every libxfce4panel-2.0.pc defines a "datadir" variable (only
# "libdir" is guaranteed), so fall back to <prefix>/share when it's
# missing instead of silently producing a rootless "/xfce4/..." path.
XFCE_DATADIR := $(shell $(PKG_CONFIG) --variable=datadir libxfce4panel-2.0)
ifeq ($(XFCE_DATADIR),)
XFCE_DATADIR := $(shell $(PKG_CONFIG) --variable=prefix libxfce4panel-2.0)/share
endif

# Where `sudo make install` puts the bundled default icons so every
# user account on the machine can see them - baked into the binary at
# compile time so caffeine-icons.c knows where to look at runtime.
SYSTEM_ICON_DIR = $(XFCE_DATADIR)/xfce4-caffeine-plugin/icons

# System-wide translation directory, baked into the binary as the
# fallback. Per-user installs go to ~/.local/share/locale instead, which
# the plugin finds on its own at runtime (see caffeine_init_translations),
# so the compiled-in path never depends on how you installed.
SYSTEM_LOCALEDIR = $(XFCE_DATADIR)/locale

CFLAGS += -Wall -O2 -DCAFFEINE_SYSTEM_ICON_DIR='"$(SYSTEM_ICON_DIR)"' \
          $(shell $(PKG_CONFIG) --cflags libxfce4panel-2.0 gtk+-3.0 gio-2.0 libxfconf-0)
LIBS   += $(shell $(PKG_CONFIG) --libs libxfce4panel-2.0 gtk+-3.0 gio-2.0 libxfconf-0) -lm

# Source layout: code lives in panel-plugin/, build artifacts go in build/
SRCDIR   = panel-plugin
BUILDDIR = build

# UID 0 means we're running as root (typically via sudo): install
# system-wide instead of into $(HOME), so the plugin (and its default
# icons) show up for every user on the machine. See the warning in the
# install target below.
UID := $(shell id -u)

ifeq ($(UID),0)
PLUGIN_LIBDIR = $(shell $(PKG_CONFIG) --variable=libdir libxfce4panel-2.0)/xfce4/panel/plugins
DESKTOP_DIR   = $(XFCE_DATADIR)/xfce4/panel/plugins
ICONS_DIR     = $(SYSTEM_ICON_DIR)
LOCALEDIR     = $(SYSTEM_LOCALEDIR)
else
# Per-user install paths (no sudo needed). Xfce 4.20+ uses
# "panel/plugins", not the older "panel-plugins".
PLUGIN_LIBDIR = $(HOME)/.local/lib/xfce4/panel/plugins
DESKTOP_DIR   = $(HOME)/.local/share/xfce4/panel/plugins
ICONS_DIR     = $(HOME)/.config/xfce4-caffeine-plugin/icons
LOCALEDIR     = $(HOME)/.local/share/locale
endif

# Gettext domain and the system-wide translation directory (fallback;
# a per-user install is found at runtime, so no `make clean` is needed
# when switching between `make install` and `sudo make install`).
CFLAGS += -DGETTEXT_PACKAGE='"$(GETTEXT_PACKAGE)"' -DLOCALEDIR='"$(SYSTEM_LOCALEDIR)"'

DEFAULT_ICONS_SRC = $(SRCDIR)/icons

# Translations: every po/<lang>.po becomes build/<lang>.mo
PODIR   = $(SRCDIR)/po
POFILES = $(wildcard $(PODIR)/*.po)
LANGS   = $(patsubst $(PODIR)/%.po,%,$(POFILES))
MOFILES = $(patsubst $(PODIR)/%.po,$(BUILDDIR)/%.mo,$(POFILES))

SRC = $(SRCDIR)/caffeine.c $(SRCDIR)/caffeine-prefs.c $(SRCDIR)/caffeine-icons.c
OBJ = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRC))
TARGET = $(BUILDDIR)/lib$(PLUGIN_NAME).so

HEADERS = $(SRCDIR)/caffeine.h $(SRCDIR)/caffeine-prefs.h $(SRCDIR)/caffeine-icons.h

all: $(TARGET) $(MOFILES)

$(TARGET): $(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(HEADERS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILDDIR)/%.mo: $(PODIR)/%.po | $(BUILDDIR)
	msgfmt --check -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

install: all
ifeq ($(UID),0)
	@echo "############################################################"
	@echo "# WARNING: running as root - installing SYSTEM-WIDE."
	@echo "#   Plugin:  $(DESTDIR)$(PLUGIN_LIBDIR)"
	@echo "#   Desktop: $(DESTDIR)$(DESKTOP_DIR)"
	@echo "#   Icons:   $(DESTDIR)$(ICONS_DIR)"
	@echo "# This makes the plugin AND its default icons available to"
	@echo "# every user on this machine, and requires root to"
	@echo "# uninstall/upgrade later. A user can still override the"
	@echo "# icons for just their own account via"
	@echo "# ~/.config/xfce4-caffeine-plugin/icons."
	@echo "# For a normal single-user install instead, run without"
	@echo "# sudo/root:  make install"
	@echo "############################################################"
endif
	install -d $(DESTDIR)$(PLUGIN_LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(PLUGIN_LIBDIR)/
	install -d $(DESTDIR)$(DESKTOP_DIR)
	install -m 644 caffeine-plugin.desktop $(DESTDIR)$(DESKTOP_DIR)/
	$(MAKE) install-default-icons
	@if [ -z "$(POFILES)" ]; then \
		echo "WARNING: no $(PODIR)/*.po files found next to the Makefile - translations were NOT installed"; \
	fi
	$(MAKE) install-translations

# Installs each compiled translation as
# $(LOCALEDIR)/<lang>/LC_MESSAGES/$(GETTEXT_PACKAGE).mo
install-translations: $(MOFILES)
	@for lang in $(LANGS); do \
		install -d "$(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES"; \
		install -m 644 "$(BUILDDIR)/$$lang.mo" \
			"$(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/$(GETTEXT_PACKAGE).mo"; \
		echo "installed translation $$lang"; \
	done

# Copies each bundled default icon into ICONS_DIR unless it already
# exists there, so existing customizations are never overwritten.
# $(DESTDIR) is intentionally omitted for the per-user (non-root) case -
# those are per-user runtime files a package manager shouldn't own; for
# the root case ICONS_DIR is already a proper system path.
install-default-icons:
	install -d $(ICONS_DIR)
	@for f in $(DEFAULT_ICONS_SRC)/*.png; do \
		[ -e "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		if [ -f "$(ICONS_DIR)/$$name" ]; then \
			echo "keeping existing $(ICONS_DIR)/$$name (not overwritten)"; \
		else \
			install -m 644 "$$f" "$(ICONS_DIR)/$$name"; \
			echo "installed default icon $(ICONS_DIR)/$$name"; \
		fi; \
	done

uninstall:
	rm -f $(DESTDIR)$(PLUGIN_LIBDIR)/lib$(PLUGIN_NAME).so
	rm -f $(DESTDIR)$(DESKTOP_DIR)/caffeine-plugin.desktop
	@for lang in $(LANGS); do \
		rm -f "$(DESTDIR)$(LOCALEDIR)/$$lang/LC_MESSAGES/$(GETTEXT_PACKAGE).mo"; \
	done
	@echo "Note: not removing $(ICONS_DIR) - it may still be in use."
	@echo "Remove it yourself with: rm -rf $(ICONS_DIR)"

clean:
	rm -rf $(BUILDDIR)

# Maintainer targets (need the `gettext` package):
#   make pot        - regenerate po/$(GETTEXT_PACKAGE).pot from the sources
#   make update-po  - merge the fresh .pot into every po/<lang>.po
pot:
	xgettext --from-code=UTF-8 -k_ -kN_ --package-name=$(GETTEXT_PACKAGE) \
		-o $(PODIR)/$(GETTEXT_PACKAGE).pot $(SRC)

update-po: pot
	@for po in $(POFILES); do \
		echo "updating $$po"; \
		msgmerge --update --backup=none "$$po" $(PODIR)/$(GETTEXT_PACKAGE).pot; \
	done

.PHONY: all install install-default-icons install-translations uninstall clean pot update-po