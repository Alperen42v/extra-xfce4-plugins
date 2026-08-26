PLUGIN_NAME = nightlight
SRC = panel-plugin/nightlight.c panel-plugin/nightlight-prefs.c
BUILD_DIR = build
TARGET = $(BUILD_DIR)/lib$(PLUGIN_NAME).so

PKGS = gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 x11 xrandr

CFLAGS += -Wall -O2 -fPIC $(shell pkg-config --cflags $(PKGS))
LIBS   += -lm $(shell pkg-config --libs $(PKGS))

# Install location is chosen automatically based on who runs "make install":
#   - normal user  -> installs only for that user, no root needed
#   - root / sudo  -> installs system-wide, available to every user
UID := $(shell id -u)

ifeq ($(UID),0)
LIB_DIR      = $(DESTDIR)/usr/lib/xfce4/panel/plugins
DESKTOP_DIR  = $(DESTDIR)/usr/share/xfce4/panel/plugins
INSTALL_SCOPE = system-wide (all users)
else
LIB_DIR      = $(HOME)/.local/lib/xfce4/panel/plugins
DESKTOP_DIR  = $(HOME)/.local/share/xfce4/panel/plugins
INSTALL_SCOPE = current user only
endif

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $(SRC) $(LIBS)

# Build first as a normal user ("make"), then either:
#   make install        -> installs for you only
#   sudo make install   -> installs for every user on the system
install: $(TARGET)
ifeq ($(UID),0)
	@echo "############################################################"
	@echo "# WARNING: running as root - installing SYSTEM-WIDE."
	@echo "#   Plugin:  $(LIB_DIR)/lib$(PLUGIN_NAME).so"
	@echo "#   Desktop: $(DESKTOP_DIR)/nightlight.desktop"
	@echo "# This makes the plugin available to every user on this"
	@echo "# machine, and requires root to uninstall/upgrade later."
	@echo "# For a normal single-user install instead, run without"
	@echo "# sudo/root:  make install"
	@echo "############################################################"
endif
	mkdir -p $(LIB_DIR)
	mkdir -p $(DESKTOP_DIR)
	install -m 755 $(TARGET) $(LIB_DIR)/lib$(PLUGIN_NAME).so
	install -m 644 nightlight.desktop $(DESKTOP_DIR)/nightlight.desktop
	@echo ""
	@echo "Installed: $(INSTALL_SCOPE)"
	@echo "  Library:  $(LIB_DIR)/lib$(PLUGIN_NAME).so"
	@echo "  Desktop:  $(DESKTOP_DIR)/nightlight.desktop"
	@echo "To add it to the panel: right-click the panel -> Panel -> Add New Items -> search 'Night Light'."
	@echo "To refresh the panel cache: xfce4-panel -r"

# Uninstalls from the same scope the current invocation resolves to
# (run plain "make uninstall" to remove the per-user copy, "sudo make uninstall"
# to remove the system-wide copppy
uninstall:
	rm -f $(LIB_DIR)/lib$(PLUGIN_NAME).so
	rm -f $(DESKTOP_DIR)/nightlight.desktop
	@echo "Uninstalled: $(INSTALL_SCOPE)"

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all install uninstall clean