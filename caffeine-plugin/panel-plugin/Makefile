PLUGIN_NAME = caffeine

PKG_CONFIG   = pkg-config
CFLAGS      += -Wall -O2 $(shell $(PKG_CONFIG) --cflags libxfce4panel-2.0 gtk+-3.0 gio-2.0 libxfconf-0)
LIBS        += $(shell $(PKG_CONFIG) --libs libxfce4panel-2.0 gtk+-3.0 gio-2.0 libxfconf-0) -lm

# Per-user install paths (no sudo needed, doesn't touch the system).
# Xfce 4.20 looks in $HOME/.local/lib/xfce4/panel/plugins for modules
# and in $HOME/.local/share/xfce4/panel/plugins for .desktop files
# (note: "panel/plugins", NOT "panel-plugins" - this changed in 4.20).
PLUGIN_LIBDIR = $(HOME)/.local/lib/xfce4/panel/plugins
DESKTOP_DIR   = $(HOME)/.local/share/xfce4/panel/plugins

# Where make install copies the *default* icons from (bundled in this
# source tree) to. This is the same fixed folder caffeine-icons.c reads
# custom icons from at runtime - see caffeine-icons.h.
DEFAULT_ICONS_SRC = icons
USER_ICONS_DIR    = $(HOME)/.config/xfce4-caffeine-plugin/icons

SRC = caffeine.c caffeine-prefs.c caffeine-icons.c
OBJ = $(SRC:.c=.o)
TARGET = lib$(PLUGIN_NAME).so

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -shared -o $@ $(OBJ) $(LIBS)

%.o: %.c caffeine.h caffeine-prefs.h caffeine-icons.h
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

install: all install-default-icons
	install -d $(DESTDIR)$(PLUGIN_LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(PLUGIN_LIBDIR)/
	install -d $(DESTDIR)$(DESKTOP_DIR)
	install -m 644 ../caffeine-plugin.desktop $(DESTDIR)$(DESKTOP_DIR)/

# Copies each bundled default icon into the user's icon folder, but only
# if that exact file isn't already there. This is deliberately per-file,
# not "only if the folder is empty": if the user only customized off.png,
# a later `make install` (e.g. after a git pull with a new on-03.png)
# still adds the new default frame without touching their off.png.
# $(DESTDIR) is intentionally omitted here - this installs into the
# invoking user's actual home config dir, not a packaging staging root,
# since these are per-user runtime files the plugin reads at runtime,
# not files a package manager should own.
install-default-icons:
	install -d $(USER_ICONS_DIR)
	@for f in $(DEFAULT_ICONS_SRC)/*.png; do \
		[ -e "$$f" ] || continue; \
		name=$$(basename "$$f"); \
		if [ -f "$(USER_ICONS_DIR)/$$name" ]; then \
			echo "keeping existing $(USER_ICONS_DIR)/$$name (not overwritten)"; \
		else \
			install -m 644 "$$f" "$(USER_ICONS_DIR)/$$name"; \
			echo "installed default icon $(USER_ICONS_DIR)/$$name"; \
		fi; \
	done

uninstall:
	rm -f $(DESTDIR)$(PLUGIN_LIBDIR)/$(TARGET)
	rm -f $(DESTDIR)$(DESKTOP_DIR)/caffeine-plugin.desktop
	@echo "Note: not removing $(USER_ICONS_DIR) - it may contain user customizations."
	@echo "Remove it yourself with: rm -rf $(USER_ICONS_DIR)"

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all install install-default-icons uninstall clean