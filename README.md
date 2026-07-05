# Cosmic Shift

A [redshift](https://github.com/jonls/redshift)-style color temperature
daemon for **Pop!_OS / COSMIC** — and any other Wayland compositor.

Classic redshift only speaks X11 gamma APIs, so it does nothing on a
Wayland session. Cosmic Shift is a small, dependency-light C program that
warms your screen at night the Wayland way.

## How it works

Cosmic Shift has two backends and picks the best one automatically:

| Backend   | Mechanism | Where it's used |
|-----------|-----------|-----------------|
| `gamma`   | `wlr-gamma-control-unstable-v1` hardware gamma tables (same as gammastep/wlsunset) | sway, Hyprland, river, niri — and COSMIC as soon as System76 ships gamma support |
| `overlay` | A full-screen, click-through, warm-tinted layer-shell surface on every output | **COSMIC today**, which doesn't implement any gamma protocol yet |

The overlay color is derived from the same blackbody curve redshift uses.
True gamma tables multiply each pixel per channel; an alpha-blended
overlay mathematically cannot, so Cosmic Shift uses the least-squares
affine approximation of the gamma result: mid-tones land on exactly the
color hardware gamma would give, while the residual error is split
between a slight lift of blacks and a slight dimming of whites. In
practice the screen gets warmer *and* a little darker, like a real night
light. The overlay is completely click-through and never takes focus.
Because the backend is probed at startup, the moment your COSMIC update
gains gamma support Cosmic Shift silently upgrades itself to the real
thing.

Overlay tips:

- An overlay cannot subtract blue outright — blue is only reduced
  *relative* to red and green, either by painting warmth on (yellow cast)
  or by darkening the whole picture. `-s` picks the balance: try
  `-s 0.25` if the yellow cast bothers you, `-s 0` for pure dimming with
  no color at all.
- Very low temperatures amplify the trade-off. 3200–4500 K is the sweet
  spot; below ~3000 K expect dark content to look brownish.
- Add `-b 0.85` if you want the screen dimmer still.

Wayland restores state when a client disconnects, so Cosmic Shift runs
until stopped; `Ctrl+C` (or `systemctl --user stop cosmic-shift`) restores
normal colors instantly.

## Installing on Pop!_OS

```sh
sudo apt install build-essential pkg-config libwayland-dev
git clone https://github.com/sewalton/cosmic-shift.git
cd cosmic-shift
make
make install PREFIX=~/.local
```

That installs the `cosmic-shift` daemon, the `cosmic-shift-gtk` GUI and a
"Cosmic Shift" entry in your app library — no sudo needed, everything
lives in `~/.local`. (System-wide instead: `sudo make install`.)

The GUI needs `python3-gi` and GTK 4, both preinstalled on Pop!_OS. All
Wayland protocol XMLs are vendored in `protocol/`, so there are no other
build dependencies. Works on any Wayland compositor: COSMIC uses the
overlay backend (see below), while sway/Hyprland/river get true hardware
gamma.

Uninstall with `make uninstall PREFIX=~/.local`.

## GUI

`cosmic-shift-gtk` (installed alongside the daemon, and available as
"Cosmic Shift" in your app library) is a redshift-gtk-style control
panel: sliders for temperature, tint and brightness that update the
screen live as you drag, an on/off switch, and a "Start at login" switch
that manages the systemd user service for you. Settings persist in
`~/.config/cosmic-shift.conf`.

While running, the app shows an icon in the system tray (COSMIC's
Status Area applet, or any StatusNotifierItem host). Closing the window
minimizes to the tray — click the icon or pick "Open Cosmic Shift" from
its menu to bring the window back, or "Quit" to exit the GUI. Neither
turns off the color shift: quitting hands off to a background daemon.
Without a tray host, closing the window hands off the same way. Requires
python3-gi with GTK 4 (preinstalled on Pop!_OS).

## Usage (command line)

```sh
cosmic-shift -t 3500              # constant warm screen (Kelvin)
cosmic-shift -l 40.7:-74.0        # follow the sun in New York
cosmic-shift -l 40.7:-74.0 -T 6500:3800   # custom day/night temps
cosmic-shift -t 4000 -b 0.9       # also dim slightly
```

| Flag | Meaning | Default |
|------|---------|---------|
| `-t TEMP` | constant color temperature in Kelvin | 4500 |
| `-l LAT:LON` | location; enables automatic day/night mode | off |
| `-T DAY:NIGHT` | temperatures for automatic mode | 6500:4000 |
| `-b BRIGHT` | brightness multiplier, 0.1–1.0 | 1.0 |
| `-s TINT` | overlay tint strength, 0.0–1.0; lower = less yellow cast, more dimming | 0.5 |
| `-g GAMMA` | gamma exponent, 0.5–2.0 (gamma backend only) | 1.0 |
| `-m MODE` | backend: `auto`, `gamma`, `overlay` | auto |
| `-v` | verbose output | off |

In automatic mode the temperature is interpolated between night and day
values while the sun is between −6° (civil dusk) and +3° elevation, the
same thresholds redshift uses.

## Run at login (systemd user service)

Edit the temperatures/location in `cosmic-shift.service` to taste, then:

```sh
mkdir -p ~/.config/systemd/user
cp cosmic-shift.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now cosmic-shift
```

Stop or disable any time:

```sh
systemctl --user stop cosmic-shift      # colors restore instantly
systemctl --user disable cosmic-shift
```

## Notes

- Multi-monitor and hotplug are handled: new or resized outputs get the
  current color automatically.
- Screenshots and screen recordings will include the tint while the
  overlay backend is active (hardware gamma, by contrast, is invisible to
  capture). Stop cosmic-shift briefly if you need color-accurate captures.
- `-g` (gamma exponent) has no effect in overlay mode.

## Credits

Created by: Seth Walton

MIT licensed — see [LICENSE](LICENSE).
