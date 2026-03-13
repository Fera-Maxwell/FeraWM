CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LIBS    = -lX11
BIN     = ferawm
SRC     = ferawm.c
BUILDDIR= build

# Install paths
PREFIX      = /usr/local
BINDIR      = $(PREFIX)/bin
DESKTOPDIR  = /usr/share/xsessions
CONFDIR     = /etc/fera

.PHONY: all clean install uninstall

all: $(BUILDDIR)/$(BIN)

$(BUILDDIR)/$(BIN): $(SRC)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) -o $(BUILDDIR)/$(BIN) $(SRC) $(LIBS)
	@echo ""
	@echo "  Built: $(BUILDDIR)/$(BIN)"
	@echo "  Run:   sudo make install"
	@echo ""

clean:
	rm -rf $(BUILDDIR)

install: $(BUILDDIR)/$(BIN)
	@echo "Installing ferawm..."
	install -Dm755 $(BUILDDIR)/$(BIN)          $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 ferawm.desktop               $(DESTDIR)$(DESKTOPDIR)/ferawm.desktop
	@mkdir -p $(DESTDIR)$(CONFDIR)
	@if [ ! -f $(DESTDIR)$(CONFDIR)/ferawm.conf ]; then \
		install -Dm644 ferawm.conf $(DESTDIR)$(CONFDIR)/ferawm.conf; \
		echo "  Config installed: $(CONFDIR)/ferawm.conf"; \
	else \
		echo "  Config already exists, skipping: $(CONFDIR)/ferawm.conf"; \
	fi
	@# create user config dir
	@mkdir -p $(HOME)/.config/fera
	@if [ ! -f $(HOME)/.config/fera/ferawm.conf ]; then \
		cp ferawm.conf $(HOME)/.config/fera/ferawm.conf; \
		echo "  User config: $(HOME)/.config/fera/ferawm.conf"; \
	else \
		echo "  User config already exists, skipping"; \
	fi
	@echo ""
	@echo "  Installed to $(BINDIR)/$(BIN)"
	@echo "  Desktop entry: $(DESKTOPDIR)/ferawm.desktop"
	@echo "  User config:   ~/.config/fera/ferawm.conf"
	@echo ""

uninstall:
	rm -f  $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f  $(DESTDIR)$(DESKTOPDIR)/ferawm.desktop
	@echo "Uninstalled ferawm"
