PLUGIN_NAME = nightlight
SRC = panel-plugin/nightlight.c panel-plugin/nightlight-prefs.c
BUILD_DIR = build
TARGET = $(BUILD_DIR)/lib$(PLUGIN_NAME).so

PKGS = gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 x11 xrandr

# --- Translations (gettext) ---
# Needs the "gettext" package (msgfmt, xgettext, msgmerge).
# The languages to build/install are listed in panel-plugin/po/LINGUAS.
GETTEXT_PACKAGE = $(PLUGIN_NAME)
PO_DIR   = panel-plugin/po
LINGUAS := $(shell cat $(PO_DIR)/LINGUAS)
MO_FILES = $(foreach l,$(LINGUAS),$(BUILD_DIR)/locale/$(l)/LC_MESSAGES/$(GETTEXT_PACKAGE).mo)

CFLAGS += -Wall -O2 -fPIC $(shell pkg-config --cflags $(PKGS))
CFLAGS += -DGETTEXT_PACKAGE=\"$(GETTEXT_PACKAGE)\" -DPACKAGE_LOCALE_DIR=\"/usr/share/locale\"
LIBS   += -lm $(shell pkg-config --libs $(PKGS))

# Install location is chosen automatically based on who runs "make install":
#   - normal user  -> installs only for that user, no root needed
#   - root / sudo  -> installs system-wide, available to every user
UID := $(shell id -u)

ifeq ($(UID),0)
LIB_DIR      = $(DESTDIR)/usr/lib/xfce4/panel/plugins
DESKTOP_DIR  = $(DESTDIR)/usr/share/xfce4/panel/plugins
LOCALE_DIR   = $(DESTDIR)/usr/share/locale
INSTALL_SCOPE = system-wide (all users)
else
LIB_DIR      = $(HOME)/.local/lib/xfce4/panel/plugins
DESKTOP_DIR  = $(HOME)/.local/share/xfce4/panel/plugins
LOCALE_DIR   = $(HOME)/.local/share/locale
INSTALL_SCOPE = current user only
endif

all: $(TARGET) $(MO_FILES)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -shared -o $@ $(SRC) $(LIBS)

# .po -> .mo  (--check-accelerators makes sure "_Close"-style mnemonics survive translation)
$(BUILD_DIR)/locale/%/LC_MESSAGES/$(GETTEXT_PACKAGE).mo: $(PO_DIR)/%.po
	mkdir -p $(dir $@)
	msgfmt --check --check-accelerators=_ -o $@ $<

# Regenerate the template from the source code (run after changing any _("...") string)
pot:
	xgettext --from-code=UTF-8 -k_ -kN_ -kC_:1c,2 --add-comments=TRANSLATORS: \
		--package-name=$(PLUGIN_NAME) --msgid-bugs-address="" \
		-o $(PO_DIR)/$(GETTEXT_PACKAGE).pot -f $(PO_DIR)/POTFILES.in

# Merge the new template into every existing .po (new strings show up untranslated)
update-po: pot
	for l in $(LINGUAS); do \
		msgmerge --update --backup=none $(PO_DIR)/$$l.po $(PO_DIR)/$(GETTEXT_PACKAGE).pot; \
	done

# Build first as a normal user ("make"), then either:
#   make install        -> installs for you only
#   sudo make install   -> installs for every user on the system
install: all
ifeq ($(UID),0)
	@echo "############################################################"
	@echo "# WARNING: running as root - installing SYSTEM-WIDE."
	@echo "#   Plugin:  $(LIB_DIR)/lib$(PLUGIN_NAME).so"
	@echo "#   Desktop: $(DESKTOP_DIR)/nightlight.desktop"
	@echo "#   Locale:  $(LOCALE_DIR)/<lang>/LC_MESSAGES/$(GETTEXT_PACKAGE).mo"
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
	for l in $(LINGUAS); do \
		install -d $(LOCALE_DIR)/$$l/LC_MESSAGES; \
		install -m 644 $(BUILD_DIR)/locale/$$l/LC_MESSAGES/$(GETTEXT_PACKAGE).mo \
			$(LOCALE_DIR)/$$l/LC_MESSAGES/$(GETTEXT_PACKAGE).mo; \
	done
	@echo ""
	@echo "Installed: $(INSTALL_SCOPE)"
	@echo "  Library:  $(LIB_DIR)/lib$(PLUGIN_NAME).so"
	@echo "  Desktop:  $(DESKTOP_DIR)/nightlight.desktop"
	@echo "  Locale:   $(LOCALE_DIR)/<lang>/LC_MESSAGES/$(GETTEXT_PACKAGE).mo ($(LINGUAS))"
	@echo "To add it to the panel: right-click the panel -> Panel -> Add New Items -> search 'Night Light'."
	@echo "To refresh the panel cache: xfce4-panel -r"

# Uninstalls from the same scope the current invocation resolves to
# (run plain "make uninstall" to remove the per-user copy, "sudo make uninstall"
# to remove the system-wide copy)
uninstall:
	rm -f $(LIB_DIR)/lib$(PLUGIN_NAME).so
	rm -f $(DESKTOP_DIR)/nightlight.desktop
	for l in $(LINGUAS); do rm -f $(LOCALE_DIR)/$$l/LC_MESSAGES/$(GETTEXT_PACKAGE).mo; done
	@echo "Uninstalled: $(INSTALL_SCOPE)"

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all install uninstall clean pot update-po