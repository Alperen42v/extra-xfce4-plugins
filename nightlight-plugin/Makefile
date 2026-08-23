PLUGIN_NAME = nightlight
SRC = panel-plugin/nightlight.c panel-plugin/nightlight-prefs.c
BUILD_DIR = build
TARGET = $(BUILD_DIR)/lib$(PLUGIN_NAME).so

PKGS = gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 x11 xrandr

CFLAGS += -Wall -O2 -fPIC $(shell pkg-config --cflags $(PKGS))
LIBS   += -lm $(shell pkg-config --libs $(PKGS))

# Per-user install directories (no sudo needed)
USER_LIB_DIR   = $(HOME)/.local/lib/xfce4/panel/plugins
USER_DESKTOP_DIR = $(HOME)/.local/share/xfce4/panel/plugins

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $(SRC) $(LIBS)

install: $(TARGET)
	mkdir -p $(USER_LIB_DIR)
	mkdir -p $(USER_DESKTOP_DIR)
	install -m 755 $(TARGET) $(USER_LIB_DIR)/lib$(PLUGIN_NAME).so
	install -m 644 nightlight.desktop $(USER_DESKTOP_DIR)/nightlight.desktop
	@echo ""
	@echo "Installed (no sudo used)."
	@echo "To add it to the panel: right-click the panel -> Panel -> Add New Items -> search 'Night Light'."
	@echo "To refresh the panel cache: xfce4-panel -r"

uninstall:
	rm -f $(USER_LIB_DIR)/lib$(PLUGIN_NAME).so
	rm -f $(USER_DESKTOP_DIR)/nightlight.desktop
	@echo "Uninstalled."

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all install uninstall clean