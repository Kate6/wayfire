# [Wayfire]

[Wayfire]: https://wayfire.org

![Version](https://img.shields.io/github/v/release/WayfireWM/wayfire)
[![Matrix: #wayfire:matrix.org](https://img.shields.io/badge/matrix-%23wayfire%3Amatrix.org-blue)](https://matrix.to/#/#wayfire:matrix.org)
[![IRC: #wayfire on Libera.chat](https://img.shields.io/badge/IRC-%23wayfire%20at%20libera.chat-green)](https://web.libera.chat/#wayfire)
[![Discord](https://img.shields.io/discord/1144831589877043220?label=Discord)](https://discord.gg/5SWAxmBCUH)
[![CI](https://github.com/WayfireWM/wayfire/workflows/CI/badge.svg)](https://github.com/WayfireWM/wayfire/actions)
[![Packaging status](https://repology.org/badge/tiny-repos/wayfire.svg)](https://repology.org/project/wayfire/versions)
[![License](https://img.shields.io/github/license/WayfireWM/wayfire)](LICENSE)

###### [Get started] | [Manual] | [Configuration]

[Get started]: https://github.com/WayfireWM/wayfire/wiki/Tutorial
[Manual]: https://github.com/WayfireWM/wayfire/wiki/General
[Configuration]: https://github.com/WayfireWM/wayfire/wiki/Configuration

Wayfire is a 3D [Wayland] compositor, inspired by [Compiz] and based on [wlroots].

It aims to create a customizable, extendable and lightweight environment without sacrificing its appearance.

[![Wayfire demos](https://img.youtube.com/vi_webp/2PtNzxDsxYM/maxresdefault.webp)](https://youtube.com/playlist?list=PLb7YRKEhWEBUIoT-a29UoJW9mhfzjpNle "YouTube – Wayfire demos")
[![YouTube Play Button](https://www.iconfinder.com/icons/317714/download/png/16)](https://youtube.com/playlist?list=PLb7YRKEhWEBUIoT-a29UoJW9mhfzjpNle) · [Wayfire demos](https://youtube.com/playlist?list=PLb7YRKEhWEBUIoT-a29UoJW9mhfzjpNle)

[Wayland]: https://wayland.freedesktop.org
[wlroots]: https://github.com/swaywm/wlroots
[Compiz]: https://launchpad.net/compiz

## Local additions

> **Note:** this section documents work on local branches, not released
> Wayfire features. Everything here lives in this tree; the four items are
> independent of one another and each can be taken on its own.

### `eis` plugin (input capture)

`plugins/eis/` implements the compositor half of
`org.freedesktop.impl.portal.InputCapture`, which lets a portal hand the
compositor's keyboard and pointer to a remote machine over the network — the
mechanism Deskflow uses for its KVM portal.

Wayfire acts as the [EIS] server. It owns the EI devices and region geometry,
gives each capture session an `eis` connection over a file descriptor, and
translates pointer barriers between portal coordinates and output-space
coordinates. The public portal API is *not* implemented here; the plugin
exposes a private bus interface, `org.wayfire.Eis`, which the InputCapture
backend in xdg-desktop-portal-wlr sits on top of.

Notable behaviour:

- one `struct eis *` per capture session
- the session's zone is the union of the enabled output work areas, and
  `ZonesChanged` is emitted whenever it moves
- barrier IDs are compositor-owned; the backend maps them in both directions
- the cursor is hidden while capture is active, since the pointer is driving
  the remote machine, and held keys/buttons are released on deactivation so
  modifiers do not stick

Exercised end to end against a real Deskflow client: session creation and
`Start`, the EI handshake yielding pointer and keyboard devices, barriers for
every outer edge, barrier crossing handing capture to the remote machine, and
clean teardown on `Stop`.

Needs `libei` (pkg-config `libeis-1.0`, which provides the server half) and
`libsystemd`.

[EIS]: https://gitlab.freedesktop.org/libei/libei

### `focus-follows-mouse` plugin

Moves keyboard focus to whichever window the pointer enters. Wayfire has no
built-in option for this — the only focus-related keys in the metadata are
`core/focus_buttons`, `core/focus_button_with_modifiers` and `simple-tile`'s
`key_focus_*`, none of which track the pointer — so it is a plugin.

Two behaviours are suppressed on purpose:

- **While any pointer button is held, focus is frozen.** Every pointer-initiated
  grab (window move, resize, drag-and-drop) holds a button for its duration, so
  this one flag prevents the failure mode where dragging a window across the
  screen keeps re-focusing everything it passes over and the drag fights the
  focus change.
- **Popups and layer surfaces are skipped** even when they report themselves
  focusable. Focusing one pulls the keyboard to the menu instead of the window
  beneath it.

Focus does **not** raise by default. `window_manager_t::focus_request()` always
calls `view_bring_to_front()`, which is not exposed in the public header and so
cannot be undone afterwards; the default path uses `seat->focus_view()` instead.
The trade-off is that `focus_view()` does not emit `view_focus_request_signal`,
so a plugin that vetoes focus changes will not see requests from this one. Set
`raise = true` for the old behaviour.

Options, under `[focus-follows-mouse]`:

| Option | Default | Meaning |
|---|---|---|
| `delay` | `0` | milliseconds to rest before focus moves; `0` is immediate |
| `cross_output` | `true` | whether focus may follow the pointer to another output |
| `raise` | `false` | bring the newly focused window to the front |

### Smart window placement

Adds `mode = smart` to the `place` plugin, after Compiz's `placeSmart()`
(`plugins/place.c`). Where cascade walks fixed offsets until something fits,
smart scores each candidate position by how much it would overlap existing
windows and takes the least-bad one, stepping past each obstacle in turn.

Overlapping a window that is *above* the new one counts sixteen times as much
as an ordinary window, and a window *below* counts nothing — so the result
slides underneath things rather than over them. Wayfire does not expose
per-view above/below state, so the scene layer is used as the closest
equivalent: `TOP`-layer (always-on-top) views weigh 16, `WORKSPACE` views weigh
1, `BOTTOM`-layer views weigh 0.

The original credits SmartPlacement by Cristian Tibirna, adapted through kwm,
kwin, fvwm and xfce before reaching Compiz. Windows at least as large as the
workarea fall back to centring rather than running the scan, which would
otherwise iterate pointlessly.

### Popups parented to a layer-shell surface can take keyboard focus

Without this, panel menus cannot be typed into: clicking into one does nothing
and keystrokes go to whichever window had been focused before. The LXQt panel
and the wf-shell panel both exhibited it.

The cause was a mutual deadlock. `wayfire_xdg_popup::get_keyboard_focus_surface()`
returns `nullptr` while `parent_allows_keyboard_focus()` is false, and that is
false while the parent has no focus surface of its own. A layer-shell panel has
none, because clients set its keyboard interactivity to `none` deliberately — a
panel should not swallow keystrokes. Neither side could therefore be focused.

Clients rely on the compositor here rather than on the panel holding focus.
lxqt-panel matches `XDG_CURRENT_DESKTOP` against `kde|kwin|labwc|wayfire|hyprland`
and selects `KeyboardInteractivityNone` only for those compositors, explicitly
expecting the compositor to focus the child popup instead.

The fix treats a non-toplevel parent — i.e. a layer-shell surface — as
permitting focus. Toplevels always take focus on click, so they are not the case
that can deadlock.

## Dependencies

### Wayfire Dependencies

These are the dependencies needed for building Wayfire.

- [Cairo](https://cairographics.org)
- [Pango](https://pango.gnome.org/) and PangoCairo
- [FreeType](https://freetype.org)
- [GLM](https://glm.g-truc.net)
- [libdrm](https://dri.freedesktop.org/wiki/DRM/)
- [libevdev](https://freedesktop.org/wiki/Software/libevdev/)
- [libGL](https://mesa3d.org)
- [libinput](https://freedesktop.org/wiki/Software/libinput/)
- [libjpeg](https://libjpeg-turbo.org)
- [libpng](http://libpng.org/pub/png/libpng.html)
- [libxkbcommon](https://xkbcommon.org)
- [libxml2](http://xmlsoft.org/)
- [Pixman](https://pixman.org)
- [pkg-config](https://freedesktop.org/wiki/Software/pkg-config/)
- [Wayland](https://wayland.freedesktop.org)
- [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols)
- [wf-config](https://github.com/WayfireWM/wf-config)
- [wlroots](https://github.com/swaywm/wlroots)

### wlroots Dependencies

These are the dependencies needed for building wlroots, and should be installed before building it.
They are relevant for cases when the system doesn't have a version of wlroots installed.

#### DRM Backend (required)

- [libdisplay-info-dev](https://gitlab.freedesktop.org/emersion/libdisplay-info)
- [hwdata-dev](https://github.com/vcrhonek/hwdata)

#### GLES2 renderer (required)
- [libglvnd](https://gitlab.freedesktop.org/glvnd/libglvnd)
- [mesa](https://gitlab.freedesktop.org/mesa/mesa) (with libEGL and gbm support)

#### Libinput Backend (required)
- [libinput](https://gitlab.freedesktop.org/libinput/libinput)

#### Session Provider (required)

- libudev (via [systemd](https://systemd.io/) **or** other providers)
- [seatd](https://git.sr.ht/~kennylevinsen/seatd)

#### XWayland Support (optional)

- [xcb](https://xcb.freedesktop.org/)
- [xcb-composite](https://xorg.freedesktop.org/wiki/)
- [xcb-render](https://xorg.freedesktop.org/wiki/)
- [xcb-xfixes](https://xorg.freedesktop.org/wiki/)

#### X11 Backend (optional)

- [xcb](https://xcb.freedesktop.org/)
- [x11-xcb](https://xcb.freedesktop.org/)
- [xcb-xinput](https://xorg.freedesktop.org/wiki/)
- [xcb-xfixes](https://xorg.freedesktop.org/wiki/)

## Installation

The easiest way to install Wayfire, wf-shell and WCM to get a functional desktop is to use the [install scripts](https://github.com/WayfireWM/wf-install).

Alternatively, you can build from source:

``` sh
meson build
ninja -C build
sudo ninja -C build install
```

**Note**: `wf-config` and `wlroots` can be built as submodules, by specifying
`-Duse_system_wfconfig=disabled` and `-Duse_system_wlroots=disabled` options to `meson`.
This is the default if they are not present on your system.

Installing [wf-shell](https://github.com/WayfireWM/wf-shell) is recommended for a complete experience.

External plugins can be installed either manually or with `wayfire-plugin`, for example:

```sh
wayfire-plugin install https://github.com/soreau/pixdecor
```

See [Managing External Plugins](docs/wayfire-plugin.md) for more information.

###### Arch Linux

[wayfire](https://aur.archlinux.org/packages/wayfire/) and [wayfire-git] are available in the [AUR].

``` sh
yay -S wayfire
```

[AUR]: https://aur.archlinux.org
[wayfire-git]: https://aur.archlinux.org/packages/wayfire-git/

###### Exherbo

``` sh
cave resolve -x wayfire
```

###### Fedora

``` sh
dnf install wayfire
```

###### FreeBSD
Install the latest release and recommended addons with
``` sh
pkg install wayfire wayfire-plugins-extra wf-shell wcm
```

###### Gentoo
Install the latest release with
```sh
emerge --ask --verbose wayfire
```
and to use the live version
```sh
emerge --ask --verbose "=gui-wm/wayfire-9999"
```

###### NixOS

Enable Wayfire in your NixOS configuration:
```nix
programs.wayfire = {
  enable = true;
  plugins = with pkgs.wayfirePlugins; [
    wcm
    wf-shell
    wayfire-plugins-extra
  ];
};
```

###### Ubuntu/Debian 13

```
apt install wayfire
```

###### Void

``` sh
xbps-install -S wayfire
```

## Configuration

Copy [`wayfire.ini`] to `~/.config/wayfire.ini` or `~/.config/wayfire/wayfire.ini`.
Before running Wayfire, you may want to change the command to start a terminal.
See the [Configuration] document for information on the options.

[`wayfire.ini`]: wayfire.ini

## Running

Run [`wayfire`][Manual] from a TTY, or via a Wayland-compatible login manager.
