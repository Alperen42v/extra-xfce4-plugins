PLUGIN_NAME = kdeconnect

PKG_CONFIG = pkg-config
PKGS       = libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gtk+-3.0 gio-2.0

# Not every libxfce4panel-2.0.pc defines a "datadir" variable (only
# "libdir" is guaranteed), so fall back to <prefix>/share when it's
# missing instead of silently producing a rootless "/xfce4/..." path.
XFCE_DATADIR := $(shell $(PKG_CONFIG) --variable=datadir libxfce4panel-2.0)
ifeq ($(XFCE_DATADIR),)
XFCE_DATADIR := $(shell $(PKG_CONFIG) --variable=prefix libxfce4panel-2.0)/share
endif

CFLAGS += -Wall -Wextra -Wno-unused-parameter -O2 $(shell $(PKG_CONFIG) --cflags $(PKGS))
LIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS))

# Source layout: code lives in panel-plugin/, build artifacts go in build/
SRCDIR   = panel-plugin
BUILDDIR = build

# UID 0 means we're running as root (typically via sudo): install
# system-wide instead of into $(HOME). See the warning in the install
# target below.
UID := $(shell id -u)

ifeq ($(UID),0)
PLUGIN_LIBDIR = $(shell $(PKG_CONFIG) --variable=libdir libxfce4panel-2.0)/xfce4/panel/plugins
DESKTOP_DIR   = $(XFCE_DATADIR)/xfce4/panel/plugins
else
# Per-user install paths (no sudo needed). Xfce 4.20+ uses
# "panel/plugins", not the older "panel-plugins".
PLUGIN_LIBDIR = $(HOME)/.local/lib/xfce4/panel/plugins
DESKTOP_DIR   = $(HOME)/.local/share/xfce4/panel/plugins
endif

DESKTOP_FILE = $(PLUGIN_NAME)-plugin.desktop

SRC    = $(SRCDIR)/plugin.c $(SRCDIR)/kdc-client.c $(SRCDIR)/share-window.c \
         $(SRCDIR)/preferences.c
OBJ    = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRC))
TARGET = $(BUILDDIR)/lib$(PLUGIN_NAME).so

HEADERS = $(SRCDIR)/kdc-client.h $(SRCDIR)/share-window.h $(SRCDIR)/preferences.h

# Without this, a missing file only shows up as the cryptic
#   "No rule to make target 'build/plugin.o'".
MISSING := $(filter-out $(wildcard $(SRC) $(HEADERS)),$(SRC) $(HEADERS))
ifneq ($(MISSING),)
ifneq ($(filter-out clean uninstall,$(or $(MAKECMDGOALS),all)),)
$(error Missing source file(s): $(MISSING) -- every source file must be inside $(SRCDIR)/, next to the folder that contains this Makefile)
endif
endif

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(HEADERS) | $(BUILDDIR) check-deps
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Friendly error instead of a wall of "gtk/gtk.h: No such file" messages.
check-deps:
	@$(PKG_CONFIG) --exists $(PKGS) || { \
		echo "ERROR: missing development files for: $(PKGS)"; \
		echo "On Arch: sudo pacman -S --needed base-devel pkgconf gtk3 xfce4-panel libxfce4util xfconf"; \
		exit 1; }

install: all
ifeq ($(UID),0)
	@echo "############################################################"
	@echo "# WARNING: running as root - installing SYSTEM-WIDE."
	@echo "#   Plugin:  $(DESTDIR)$(PLUGIN_LIBDIR)"
	@echo "#   Desktop: $(DESTDIR)$(DESKTOP_DIR)"
	@echo "# This makes the plugin available to every user on this"
	@echo "# machine, and requires root to uninstall/upgrade later."
	@echo "# For a normal single-user install instead, run without"
	@echo "# sudo/root:  make install"
	@echo "############################################################"
endif
	install -d $(DESTDIR)$(PLUGIN_LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(PLUGIN_LIBDIR)/
	install -d $(DESTDIR)$(DESKTOP_DIR)
	sed 's|^X-XFCE-Module-Path=.*|X-XFCE-Module-Path=$(PLUGIN_LIBDIR)|' $(DESKTOP_FILE) \
		> $(DESTDIR)$(DESKTOP_DIR)/$(DESKTOP_FILE)
	chmod 644 $(DESTDIR)$(DESKTOP_DIR)/$(DESKTOP_FILE)
	@echo "Installed. Restart the panel with:  xfce4-panel -r"

uninstall:
	rm -f $(DESTDIR)$(PLUGIN_LIBDIR)/lib$(PLUGIN_NAME).so
	rm -f $(DESTDIR)$(DESKTOP_DIR)/$(DESKTOP_FILE)
	@echo "Removed. Restart the panel with:  xfce4-panel -r"

clean:
	rm -rf $(BUILDDIR)

.PHONY: all check-deps install uninstall clean