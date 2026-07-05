PREFIX ?= /usr/local
APP_ID = io.cosmicshift.Settings
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
WAYLAND_SCANNER = wayland-scanner

WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS := $(shell pkg-config --libs wayland-client)

# xdg-shell is only here because wlr-layer-shell's generated code references
# the xdg_popup interface symbol.
PROTOCOLS = \
	wlr-gamma-control-unstable-v1 \
	wlr-layer-shell-unstable-v1 \
	viewporter \
	single-pixel-buffer-v1 \
	xdg-shell

PROTO_C = $(addsuffix -client-protocol.c,$(PROTOCOLS))
PROTO_H = $(addsuffix -client-protocol.h,$(PROTOCOLS))

all: cosmic-shift

%-client-protocol.h: protocol/%.xml
	$(WAYLAND_SCANNER) client-header $< $@

%-client-protocol.c: protocol/%.xml
	$(WAYLAND_SCANNER) private-code $< $@

cosmic-shift: cosmic-shift.c $(PROTO_C) $(PROTO_H)
	$(CC) $(CFLAGS) $(WAYLAND_CFLAGS) -o $@ cosmic-shift.c $(PROTO_C) $(WAYLAND_LIBS) -lm

install: cosmic-shift
	install -Dm755 cosmic-shift $(DESTDIR)$(PREFIX)/bin/cosmic-shift
	install -Dm755 cosmic-shift-gtk $(DESTDIR)$(PREFIX)/bin/cosmic-shift-gtk
	install -Dm644 $(APP_ID).desktop \
		$(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop
	install -Dm644 icons/$(APP_ID).svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg
	install -Dm644 icons/$(APP_ID)-symbolic.svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/symbolic/apps/$(APP_ID)-symbolic.svg
	rm -f $(DESTDIR)$(PREFIX)/share/applications/cosmic-shift.desktop
	-gtk-update-icon-cache -f $(DESTDIR)$(PREFIX)/share/icons/hicolor 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cosmic-shift \
		$(DESTDIR)$(PREFIX)/bin/cosmic-shift-gtk \
		$(DESTDIR)$(PREFIX)/share/applications/$(APP_ID).desktop \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/$(APP_ID).svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/symbolic/apps/$(APP_ID)-symbolic.svg

clean:
	rm -f cosmic-shift $(PROTO_C) $(PROTO_H)

.PHONY: all install uninstall clean
