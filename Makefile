CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra
PKGS = gtk4 gio-2.0
CORE = src/json.c src/util.c src/library.c src/steam.c src/proton.c
PREFIX ?= /usr/local

ghostlauncher: $(CORE) src/main.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ $^ $$(pkg-config --libs $(PKGS))

test: $(CORE) tests/test_core.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined $$(pkg-config --cflags gio-2.0) -o tests/test_core $^ $$(pkg-config --libs gio-2.0)
	cd tests && GL_HOME=$$(mktemp -d) ASAN_OPTIONS=detect_leaks=0 ./test_core; r=$$?; rm -f test_core; exit $$r

install: ghostlauncher
	install -Dm755 ghostlauncher $(DESTDIR)$(PREFIX)/bin/ghostlauncher
	install -Dm644 ghostlauncher.desktop $(DESTDIR)$(PREFIX)/share/applications/ghostlauncher.desktop

clean:
	rm -f ghostlauncher tests/test_core

.PHONY: test install clean
