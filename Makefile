# kshot -- plain g++ + pkg-config; no build-system ceremony for six files.
PREFIX ?= /usr/local
DESTDIR ?=
CXX ?= g++
CXXFLAGS ?= -O2 -Wall -Wextra

# Flags required regardless of what the distro passes in.
#
# makepkg REPLACES CXXFLAGS wholesale rather than appending, which silently
# dropped -fPIC and broke every clean package build:
#   copy relocation against non-copyable protected symbol _ZTI7QObject@@Qt_6
# Qt6 exports QObject's typeinfo as a protected symbol, and a non-PIE
# executable cannot emit a copy relocation against one. So -fPIC/-pie are
# appended unconditionally instead of living in the default CXXFLAGS.
REQFLAGS := -std=c++20 -fPIC
REQLDFLAGS := -pie
# LayerShellQt places one surface on each output. xdg-shell fullscreen cannot:
# KWin puts every fullscreen window on the output the window was created on,
# which on a side-by-side layout is the left screen.
QTFLAGS := $(shell pkg-config --cflags --libs Qt6Widgets Qt6DBus Qt6Gui Qt6Core) -lLayerShellQtInterface
MOC := $(shell test -x /usr/lib/qt6/moc && echo /usr/lib/qt6/moc || \
         (test -x /usr/lib/qt6/libexec/moc && echo /usr/lib/qt6/libexec/moc || echo moc-qt6))
SRC := src/capture.cpp src/telemetry.cpp src/icons.cpp src/clipboard.cpp \
       src/overlay.cpp src/main.cpp
HDR := src/capture.h src/telemetry.h src/icons.h src/clipboard.h src/overlay.h

all: kshot

kshot: $(SRC) $(HDR) moc_overlay.cpp
	$(CXX) $(CXXFLAGS) $(REQFLAGS) $(LDFLAGS) $(REQLDFLAGS) -o $@ $(SRC) moc_overlay.cpp $(QTFLAGS)

# Overlay uses Q_OBJECT. NOTE: a bare `moc` on PATH is often Qt5's and fails
# with "generated using the moc from 5.15.x" -- resolve Qt6's explicitly.
moc_overlay.cpp: src/overlay.h
	$(MOC) src/overlay.h -o moc_overlay.cpp

# The Exec= path is LOAD-BEARING. KWin authorizes ScreenShot2 by resolving the
# caller's /proc/PID/exe and matching it against an installed .desktop file's
# Exec line. If they disagree, every capture fails with NoAuthorized -- so the
# desktop file is generated from PREFIX rather than shipped with a fixed path.
#
# Regenerated UNCONDITIONALLY (FORCE). PREFIX can change between invocations
# without kshot.desktop.in changing, and make cannot see that: it compares
# mtimes only. Without FORCE, `make install PREFIX=/usr` after a /usr/local
# build silently reuses the stale file. That is exactly how the first AUR
# package shipped broken -- binary at /usr/bin/kshot, Exec=/usr/local/bin/kshot,
# NoAuthorized on every capture.
kshot.desktop: kshot.desktop.in FORCE
	sed 's|@BINDIR@|$(PREFIX)/bin|' $< > $@

FORCE:

install: kshot kshot.desktop
	install -Dm755 kshot $(DESTDIR)$(PREFIX)/bin/kshot
	install -Dm644 kshot.desktop $(DESTDIR)$(PREFIX)/share/applications/kshot.desktop
	@echo "installed kshot to $(PREFIX)/bin/kshot"
	@command -v update-desktop-database >/dev/null 2>&1 && \
	  update-desktop-database $(PREFIX)/share/applications 2>/dev/null || true

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/kshot
	rm -f $(DESTDIR)$(PREFIX)/share/applications/kshot.desktop

clean:
	rm -f kshot kshot.desktop moc_overlay.cpp

.PHONY: all install uninstall clean FORCE
