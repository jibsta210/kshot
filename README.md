# kshot

A screenshot tool for KDE Plasma Wayland that skips the XDG portal.

Drag a region, annotate it, copy it. One fullscreen surface, no application
window, no round-trip through `xdg-desktop-portal-kde`.

---

## Why

Most cross-platform screenshot tools capture through
`org.freedesktop.portal.Desktop`, which hands the request to
`xdg-desktop-portal-kde`, which then asks KWin anyway. That indirection is what
buys those tools portability across X11, GNOME, Sway, Windows and macOS — it is
a real engineering trade, not sloppiness.

If you only care about KDE Plasma Wayland, you can call KWin directly. kshot
invokes `org.kde.KWin.ScreenShot2` with a pipe and KWin writes raw pixels
straight into it. One D-Bus round trip.

Measured on a Dell XPS 16 (Panther Lake, Plasma 6.7.4, 3200×2001), numbers taken
from the system journal rather than a benchmark harness:

|  | portal-based tool | kshot |
|---|---|---|
| capture path | app → portal → KWin | app → KWin |
| CPU per capture | 1.28 s | — |
| wall time per capture | 1.98 s | **~50 ms** |
| process spawned per capture | 165 MB | none |

Across repeated captures, KWin's file-descriptor count returns to baseline and
its exported-dmabuf count does not move.

## What it is not

**KDE Plasma Wayland only, by construction.** kshot hardcodes
`org.kde.KWin.ScreenShot2`. It will not work on GNOME, Sway, wlroots, or X11.
If you need any of those, use [Flameshot](https://github.com/flameshot-org/flameshot)
— it is a better-rounded tool and the portal path is precisely why.

If you are happy with a screenshot *application*, KDE ships
[Spectacle](https://apps.kde.org/spectacle/), which has a full annotation editor
and is more featureful than this. kshot exists for the single-surface flow —
drag, annotate, copy, gone — not because Spectacle lacks capability.

---

## Install

Requires Qt6 (`qt6-base`), Layer Shell Qt (`layer-shell-qt`), a C++20 compiler,
and `wl-clipboard` for clipboard support.

```bash
make
sudo make install
```

Then bind `/usr/local/bin/kshot` to a key in
**System Settings → Keyboard → Shortcuts → Custom Shortcuts** (Print is the
obvious choice).

### The install path matters

KWin **allowlists** callers of `ScreenShot2`. It resolves the calling process's
`/proc/PID/exe`, searches installed `.desktop` files for one whose `Exec` matches
that binary, and reads `X-KDE-DBUS-Restricted-Interfaces` from it. Without that,
every capture returns:

```
org.kde.KWin.ScreenShot2.Error.NoAuthorized
```

So `make install` places both the binary at `/usr/local/bin/kshot` **and**
`kshot.desktop` in `/usr/share/applications/`. Move the binary without updating
the desktop file and captures stop working.

A consequence worth knowing: **a script can never be authorized for this API**,
because its `/proc/PID/exe` is the interpreter — authorizing `/usr/bin/python3`
would let every Python program on the system capture your screen silently.

---

## Usage

```
kshot                 # capture, then select and annotate
kshot --full          # whole workspace, no editor
kshot -o shot.png     # write to a specific file
kshot --cursor        # include the mouse cursor
kshot --save          # always write a file, even on Copy
```

### In the editor

| | |
|---|---|
| drag | select a region |
| drag handles / inside | resize / move the selection |
| `1`–`7` | adjust · rect · ellipse · arrow · pen · marker · text |
| `F` | toggle solid fill |
| `[` `]` | stroke width |
| `Ctrl+Z` | undo |
| `Ctrl+A` | select the whole screen |
| `Enter` / `Ctrl+C` | copy to clipboard |
| `Ctrl+S` | copy **and** save to `~/Pictures/Screenshots` |
| `Esc` | cancel |

Eight-colour palette in the toolbar. The accent colour is read from your KDE
colour scheme (`kdeglobals`) at runtime, so the UI follows your theme.

---

## Notes for anyone building on this

Three things cost real debugging time and are not obvious:

**The Wayland clipboard needs a live owner.** It is not storage — the owning
process serves the bytes when something pastes. A tool that sets the clipboard
and exits leaves nothing behind. Qt compounds this by quitting when the last
window closes, and by being unable to set a selection at all with no window
(there is no surface or input serial). kshot pipes the PNG to `wl-copy`, which
forks a holder process. That is the same approach other Wayland tools take.

**`Qt::BypassWindowManagerHint` breaks keyboard focus on Wayland.** It is an X11
concept; on Wayland the surface goes unmanaged and the compositor never focuses
it. The mouse keeps working and every keyboard shortcut silently does nothing.

**`QPainter::drawPath()` fills with the current brush.** It is not a stroke-only
call. A brush left set by earlier painting will flood any path you meant to
outline.

Toolbar icons are vector paths drawn at paint time rather than themed icons.
`QIcon::fromTheme` returns a *null* icon silently when it cannot resolve one,
which renders blank buttons with no error — a failure mode worth avoiding in a
tool that has no window to report errors in.

---

## Status

Working and in daily use, but young — written in an evening. The capture path is
measured and solid; the editor has had far less exercise. Bug reports welcome.

## Licence

MIT.
