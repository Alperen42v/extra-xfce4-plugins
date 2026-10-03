PLUGIN_NAME = extras-menu

# User-local install target -- no root/sudo required.
PREFIX ?= $(HOME)/.local
PLUGIN_LIBDIR = $(PREFIX)/lib/xfce4/panel/plugins
DESKTOP_DIR = $(PREFIX)/share/xfce4/panel/plugins

PKG_CONFIG = pkg-config
PKGS = gtk+-3.0 libxfce4panel-2.0 libxfce4ui-2 libxfce4util-1.0 libpulse libpulse-mainloop-glib gio-2.0

CFLAGS += -Wall -Wextra -O2 $(shell $(PKG_CONFIG) --cflags $(PKGS))
LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

# Panel plugins are built as shared modules, loaded via GModule.
CFLAGS += -fPIC
LDFLAGS += -shared

SRC_DIR = panel-plugin
BUILD_DIR = build

SOURCES = $(SRC_DIR)/extras-menu.c $(SRC_DIR)/popover.c $(SRC_DIR)/audio.c $(SRC_DIR)/brightness.c \
          $(SRC_DIR)/bluetooth.c $(SRC_DIR)/network.c $(SRC_DIR)/quick-actions.c $(SRC_DIR)/battery.c \
          $(SRC_DIR)/preferences/preferences.c \
          $(SRC_DIR)/preferences/quick-actions-config.c \
          $(SRC_DIR)/preferences/quick-actions-page.c
OBJECTS = $(SOURCES:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

TARGET = $(BUILD_DIR)/lib$(PLUGIN_NAME).so
DESKTOP_FILE = $(PLUGIN_NAME).desktop

.PHONY: all clean install uninstall

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# $(dir $@) because some sources live in subfolders (preferences/).
# -MMD -MP writes .d files so that editing a header rebuilds everything
# that includes it (struct layouts are shared between files here).
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS) $(LIBS)

install: all
	install -d $(PLUGIN_LIBDIR)
	install -d $(DESKTOP_DIR)
	install -m 755 $(TARGET) $(PLUGIN_LIBDIR)/lib$(PLUGIN_NAME).so
	install -m 644 $(DESKTOP_FILE) $(DESKTOP_DIR)/$(DESKTOP_FILE)
	@echo ""
	@echo "Installed. Restart the panel to pick it up:"
	@echo "  xfce4-panel -r"

uninstall:
	rm -f $(PLUGIN_LIBDIR)/lib$(PLUGIN_NAME).so
	rm -f $(DESKTOP_DIR)/$(DESKTOP_FILE)

clean:
	rm -rf $(BUILD_DIR)